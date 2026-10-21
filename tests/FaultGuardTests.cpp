// FaultGuard turns a crash inside someone else's code into a result the host can act on.
// The null write, bad instruction and divide-by-zero below really fault: on Windows they are
// structured exceptions, on Linux and macOS they are signals.
#include "FaultGuard.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(_MSC_VER)
 #include <intrin.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
 #include <csignal>
 #include <sys/wait.h>
 #include <unistd.h>
 #define ALIEN_TEST_CAN_FORK 1
#endif

using alien::fault::Fault;
using alien::fault::Kind;
using alien::fault::guarded;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

// Defeat the optimiser: it may not assume these pointers or divisors are what they look like.
static volatile int sink = 0;
static volatile std::uintptr_t zeroAddress = 0;
static volatile int zeroDivisor = 0;
static volatile std::uintptr_t lowAddress = 0x10;   // non-zero, still unmapped

static void writeTo(std::uintptr_t address) { *reinterpret_cast<volatile int*>(address) = 42; }

#if defined(ALIEN_TEST_CAN_FORK)
// Runs `body` in a child process and returns its wait status. These run before anything else in
// main() so the child has never installed the guard before it starts.
template <class Body> static int inChild(Body body) {
    std::cout.flush(); std::cerr.flush();
    const pid_t child = fork();
    if (child == 0) { alarm(20); body(); _exit(0); }   // a child that hangs is killed by SIGALRM and fails the check
    int status = 0;
    waitpid(child, &status, 0);
    return status;
}
#endif

int main() {
   #if defined(ALIEN_TEST_CAN_FORK)
    // A crash in the host's own code, outside any guarded call, must stay a crash - even after the
    // guard has contained a plugin fault on that very thread.
    {
        const int status = inChild([] {
            Fault fault;
            guarded([&] { writeTo(zeroAddress); }, fault);       // contained
            writeTo(zeroAddress);                                 // not guarded: the process must die of SIGSEGV
        });
        require(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "a fault outside a guarded call is not swallowed");
    }
    // ...and a handler the host or a crash reporter installed earlier still gets it.
    {
        const int status = inChild([] {
            struct sigaction reporter {};
            reporter.sa_handler = [](int) { _exit(77); };
            sigemptyset(&reporter.sa_mask);
            sigaction(SIGSEGV, &reporter, nullptr);
            Fault fault;
            const bool completed = guarded([&] { writeTo(zeroAddress); }, fault);   // contained: the reporter is not called
            _exit(!completed && fault.kind == Kind::accessViolation ? 0 : 6);
        });
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "contained faults do not reach an earlier handler");
    }
    {
        const int status = inChild([] {
            struct sigaction reporter {};
            reporter.sa_handler = [](int) { _exit(77); };
            sigemptyset(&reporter.sa_mask);
            sigaction(SIGSEGV, &reporter, nullptr);
            Fault fault;
            guarded([&] { writeTo(zeroAddress); }, fault);
            writeTo(zeroAddress);                                 // unguarded: chains to the earlier handler
        });
        require(WIFEXITED(status) && WEXITSTATUS(status) == 77, "an unguarded fault still reaches the handler installed before the guard");
    }
   #endif
    // Normal completion: the callable runs, true comes back, nothing is recorded.
    {
        Fault fault; int ran = 0;
        require(guarded([&] { ran = 1; }, fault) && ran == 1 && !fault.occurred() && fault.kind == Kind::none, "a call that completes reports no fault");
    }
    // A write to a null pointer: an access violation at address zero.
    {
        Fault fault;
        require(!guarded([&] { writeTo(zeroAddress); sink = 1; }, fault), "a null write is reported as a fault");
        require(fault.kind == Kind::accessViolation, "...as an access violation");
        require(fault.address == 0, "...at address zero");
        require(fault.occurred(), "...and the fault is marked as having occurred");
        require(sink == 0, "the code after the fault did not run");
    }
    // The address is reported, so the log says what the plugin touched.
    {
        Fault fault;
        require(!guarded([&] { writeTo(lowAddress); }, fault) && fault.kind == Kind::accessViolation && fault.address == 0x10, "the faulting address is recorded");
        require(fault.instruction != 0, "the faulting instruction is recorded");
    }
    // A C++ exception escaping the callable.
    {
        Fault fault;
        require(!guarded([&] { throw std::runtime_error("plugin threw"); }, fault) && fault.kind == Kind::cppException, "an exception is a fault, not a terminate");
        require(!guarded([&] { throw 42; }, fault) && fault.kind == Kind::cppException, "so is a thrown int");
    }
   #if defined(__x86_64__) || defined(_M_X64)
    // An instruction the processor does not have.
    {
        Fault fault;
        require(!guarded([&] {
           #if defined(_MSC_VER)
            __ud2();
           #else
            __builtin_trap();
           #endif
        }, fault) && fault.kind == Kind::illegalInstruction, "an illegal instruction is reported");
    }
    // Integer division by zero.
    {
        Fault fault; volatile int result = 0;
        require(!guarded([&] { result = 100 / zeroDivisor; }, fault) && fault.kind == Kind::arithmetic, "a divide by zero is reported");
        (void) result;
    }
   #endif
    // Faults can be repeated thousands of times and the guard keeps working: no handler is left
    // armed, no state leaks between calls.
    {
        Fault fault; int completed = 0, faulted = 0;
        for (int i = 0; i < 20000; ++i) {
            if (i % 3 == 0) { if (!guarded([&] { writeTo(zeroAddress); }, fault) && fault.kind == Kind::accessViolation) ++faulted; }
            else if (i % 3 == 1) { if (!guarded([&] { throw std::logic_error("x"); }, fault) && fault.kind == Kind::cppException) ++faulted; }
            else if (guarded([&] { sink = i; }, fault)) ++completed;
        }
        require(faulted == 13334 && completed == 6666, "every one of 20000 mixed calls was classified correctly");
    }
    // Guards nest: the inner one absorbs its fault and the outer call carries on.
    {
        Fault outer, inner; bool innerReturned = false, afterInner = false;
        const bool outerCompleted = guarded([&] {
            innerReturned = guarded([&] { writeTo(zeroAddress); }, inner);
            afterInner = true;
        }, outer);
        require(outerCompleted && !outer.occurred() && !innerReturned && inner.kind == Kind::accessViolation && afterInner, "a guarded call inside a guarded call");
    }
    // The fault record is replaced, not accumulated, by the next call.
    {
        Fault fault;
        guarded([&] { writeTo(zeroAddress); }, fault);
        require(fault.occurred(), "set by a faulting call");
        require(guarded([&] {}, fault) && !fault.occurred(), "cleared by the next call that completes");
    }
    // The audio thread and the message thread are different threads: every thread has its own guard.
    {
        std::atomic<int> faults{0}, bad{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&] {
                for (int i = 0; i < 3000; ++i) {
                    Fault fault;
                    if (!guarded([&] { writeTo(zeroAddress); }, fault) && fault.kind == Kind::accessViolation) faults.fetch_add(1); else bad.fetch_add(1);
                    if (!guarded([&] { sink = i; }, fault)) bad.fetch_add(1);
                }
            });
        for (auto& thread : threads) thread.join();
        require(faults.load() == 12000 && bad.load() == 0, "four threads fault independently and all are contained");
    }
    // Reports a person can read.
    {
        Fault fault;
        guarded([&] { writeTo(lowAddress); }, fault);
        const auto text = alien::fault::describe(fault);
        require(text.find("access violation") != std::string::npos, "description names the fault");
        require(text.find("0x00000010") != std::string::npos, "description includes the address");
        require(text.find("instruction") != std::string::npos, "description includes the instruction");
        require(alien::fault::describe(Fault{}) == "no fault", "no fault, nothing to say");
        // The module that holds the faulting instruction is this program.
        const auto module = alien::fault::moduleContaining(reinterpret_cast<std::uintptr_t>(&writeTo));
        require(!module.empty(), "an address inside this program belongs to a module");
        require(alien::fault::moduleContaining(0).empty(), "address zero belongs to none");
    }
   #if defined(_WIN32)
    // Windows only: running out of stack inside a plugin is survivable, more than once.
    {
        struct Recurse { static int go(int depth) { volatile char pad[512]; pad[0] = static_cast<char>(depth); return go(depth + 1) + pad[0]; } };
        for (int i = 0; i < 3; ++i) {
            Fault fault;
            require(!guarded([&] { sink = Recurse::go(0); }, fault) && fault.kind == Kind::stackOverflow, "stack overflow is contained");
        }
        Fault fault;
        require(guarded([&] { sink = 1; }, fault), "and the thread still works afterwards");
    }
   #endif
    std::cout << "PASS: access violations, bad instructions, divide by zero, exceptions, repetition, nesting and threads are contained\n";
}
