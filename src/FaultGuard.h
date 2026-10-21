#pragma once
// Runs code that belongs to someone else (a plugin) and survives it going wrong.
//
//   alien::fault::Fault fault;
//   if (!alien::fault::guarded([&] { plugin.processBlock(buffer, midi); }, fault))
//       ... the call did not complete: fault.kind says why ...
//
// What is caught
//   * Windows: structured exceptions - access violation, illegal or privileged instruction,
//     integer divide by zero, array bounds, misaligned data, in-page error and stack overflow.
//   * Linux and macOS: the signals SIGSEGV, SIGBUS, SIGILL and SIGFPE raised on the calling thread.
//   * Everywhere: any C++ exception.
// What cannot be caught: a crash on a thread the plugin started itself, heap corruption that is
// only noticed later, and the process-terminating fail-fast codes (stack buffer overrun, heap
// corruption detected by the allocator). Those still end the process; this is containment of
// the common faults, not isolation. Nothing here allocates, locks or does I/O, so it is safe on
// the audio thread. After a fault the plugin's own state is unknown: do not call it again.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <malloc.h>
#elif defined(__unix__) || defined(__APPLE__)
 #include <csetjmp>
 #include <csignal>
 #include <mutex>
 #include <dlfcn.h>
 #include <ucontext.h>
 #define ALIEN_FAULT_GUARD_POSIX 1
#endif

namespace alien::fault {

enum class Kind : std::uint8_t {
    none = 0,
    accessViolation,        // read, write or execute of memory the plugin may not touch
    illegalInstruction,
    arithmetic,             // integer divide by zero, floating point trap
    stackOverflow,
    cppException,           // an exception escaped from the plugin
    other                   // some other structured exception
};

struct Fault {
    Kind kind{Kind::none};
    std::uint32_t code{};           // Windows exception code, or the POSIX signal number
    std::uintptr_t address{};       // the address the code tried to touch (access violations)
    std::uintptr_t instruction{};   // where it happened
    bool occurred() const noexcept { return kind != Kind::none; }
    bool operator==(const Fault&) const = default;
};

inline const char* kindName(Kind kind) noexcept {
    switch (kind) {
        case Kind::none: return "no fault";
        case Kind::accessViolation: return "access violation";
        case Kind::illegalInstruction: return "illegal instruction";
        case Kind::arithmetic: return "arithmetic fault";
        case Kind::stackOverflow: return "stack overflow";
        case Kind::cppException: return "unhandled C++ exception";
        case Kind::other: return "unexpected processor exception";
    }
    return "unknown fault";
}

namespace detail {

// What a plugin call looks like once its type is erased, so the guarded body is a plain function.
using Thunk = void (*)(void*);

// The callable runs inside this. A C++ exception is turned into a Fault here, never allowed to reach
// the structured-exception frame (or, on the audio thread, std::terminate).
template <class F> void invoke(void* context) {
    auto& state = *static_cast<std::pair<F*, Fault*>*>(context);
    try { (*state.first)(); }
    catch (...) { state.second->kind = Kind::cppException; }
}

#if defined(_WIN32)

inline int filter(unsigned long code, EXCEPTION_POINTERS* info, Fault* fault) noexcept {
    Kind kind;
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: kind = Kind::accessViolation; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION: kind = Kind::illegalInstruction; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW: case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION: case EXCEPTION_FLT_OVERFLOW: kind = Kind::arithmetic; break;
        case EXCEPTION_STACK_OVERFLOW: kind = Kind::stackOverflow; break;
        default: return EXCEPTION_CONTINUE_SEARCH;   // C++ exceptions, debugger breakpoints, anything not ours
    }
    fault->kind = kind;
    fault->code = static_cast<std::uint32_t>(code);
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        fault->instruction = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && info->ExceptionRecord->NumberParameters >= 2)
            fault->address = static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Only plain data lives in this function: a function that uses __try cannot also unwind C++ objects.
inline bool run(Thunk thunk, void* context, Fault* fault) noexcept {
    __try {
        thunk(context);
        return fault->kind == Kind::none;
    } __except (filter(GetExceptionCode(), GetExceptionInformation(), fault)) {
        if (fault->kind == Kind::stackOverflow) _resetstkoflw();   // let the thread's guard page catch the next overflow
        return false;
    }
}

#elif defined(ALIEN_FAULT_GUARD_POSIX)

struct Frame {
    sigjmp_buf environment;
    Fault* fault;
    Frame* outer;
};
inline thread_local Frame* currentFrame = nullptr;
inline constexpr int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
inline struct sigaction previousActions[4];

inline int signalIndex(int number) noexcept {
    for (int i = 0; i < 4; ++i) if (signals[i] == number) return i;
    return -1;
}
inline void handler(int number, siginfo_t* info, void* context) {
    if (Frame* frame = currentFrame) {
        Fault& fault = *frame->fault;
        fault.kind = number == SIGSEGV || number == SIGBUS ? Kind::accessViolation
                   : number == SIGILL ? Kind::illegalInstruction : Kind::arithmetic;
        fault.code = static_cast<std::uint32_t>(number);
        fault.address = info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0;
        fault.instruction = 0;
       #if defined(__linux__) && defined(__x86_64__)
        if (context != nullptr)
            fault.instruction = static_cast<std::uintptr_t>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
       #elif defined(__linux__) && defined(__aarch64__)
        if (context != nullptr)
            fault.instruction = static_cast<std::uintptr_t>(static_cast<ucontext_t*>(context)->uc_mcontext.pc);
       #endif
        siglongjmp(frame->environment, 1);
    }
    // Not inside a guarded call: behave as if this handler were not here.
    const int index = signalIndex(number);
    if (index >= 0) {
        const struct sigaction& previous = previousActions[index];
        if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) { previous.sa_sigaction(number, info, context); return; }
        if ((previous.sa_flags & SA_SIGINFO) == 0 && previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
            previous.sa_handler(number); return;
        }
    }
    struct sigaction fallback {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(number, &fallback, nullptr);   // returning re-runs the faulting instruction, now with the default action
}
inline void install() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < 4; ++i) {
            struct sigaction action {};
            action.sa_sigaction = &handler;
            action.sa_flags = SA_SIGINFO | SA_NODEFER;   // the signal stays unblocked, so no mask needs saving or restoring
            sigemptyset(&action.sa_mask);
            sigaction(signals[i], &action, &previousActions[i]);
        }
    });
}
inline bool run(Thunk thunk, void* context, Fault* fault) noexcept {
    install();
    Frame frame{};
    frame.fault = fault; frame.outer = currentFrame;
    if (sigsetjmp(frame.environment, 0) == 0) {
        currentFrame = &frame;
        thunk(context);
        currentFrame = frame.outer;
        return fault->kind == Kind::none;
    }
    currentFrame = frame.outer;   // arrived here by siglongjmp from the handler
    return false;
}

#else

inline bool run(Thunk thunk, void* context, Fault* fault) noexcept {
    thunk(context);
    return fault->kind == Kind::none;
}

#endif
} // namespace detail

// Runs `callable`. True when it returned normally. False when it faulted; `fault` then says how.
template <class F> bool guarded(F&& callable, Fault& fault) noexcept {
    fault = {};
    std::remove_reference_t<F>* pointer = &callable;
    std::pair<std::remove_reference_t<F>*, Fault*> state{pointer, &fault};
    return detail::run(&detail::invoke<std::remove_reference_t<F>>, &state, &fault);
}

// The file name of the module (DLL or shared object) that contains `address`, or empty. Slow:
// for reports on the message thread, never the audio thread.
inline std::string moduleContaining(std::uintptr_t address) {
    if (address == 0) return {};
   #if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) || module == nullptr) return {};
    wchar_t path[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (length == 0 || length >= sizeof(path) / sizeof(path[0])) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(length), utf8.data(), bytes, nullptr, nullptr);
    return utf8;
   #elif defined(ALIEN_FAULT_GUARD_POSIX)
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(address), &info) != 0 && info.dli_fname != nullptr) return info.dli_fname;
    return {};
   #else
    return {};
   #endif
}

namespace detail {
inline std::string hex(std::uintptr_t value) {
    static const char digits[] = "0123456789ABCDEF";
    std::string text;
    do { text.insert(text.begin(), digits[value & 15u]); value >>= 4u; } while (value != 0);
    return "0x" + std::string(text.size() < 8 ? 8 - text.size() : 0, '0') + text;
}
inline std::string fileName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
} // namespace detail

// One line a person can read and paste into a bug report.
inline std::string describe(const Fault& fault) {
    if (!fault.occurred()) return "no fault";
    std::string text = kindName(fault.kind);
    if (fault.kind == Kind::accessViolation) text += " at address " + detail::hex(fault.address);
    if (fault.code != 0) text += " (code " + detail::hex(fault.code) + ")";
    if (fault.instruction != 0) {
        text += ", instruction " + detail::hex(fault.instruction);
        const auto module = moduleContaining(fault.instruction);
        if (!module.empty()) text += " in " + detail::fileName(module);
    }
    return text;
}

} // namespace alien::fault
