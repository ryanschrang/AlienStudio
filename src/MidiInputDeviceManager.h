#pragma once
#include "MidiInputEngine.h"
#include <memory>
#include <string>
#include <vector>

namespace alien {
struct MidiInputDevice { std::string identifier, name; bool operator==(const MidiInputDevice&) const = default; };
class MidiInputConnection {
public:
    virtual ~MidiInputConnection() = default;
    virtual void start() = 0;
    virtual void stop() = 0; // Must quiesce callbacks before destruction.
};
class MidiInputBackend {
public:
    virtual ~MidiInputBackend() = default;
    virtual std::vector<MidiInputDevice> available() = 0;
    virtual std::unique_ptr<MidiInputConnection> open(const std::string&, MidiInputPipe&, MidiInputPipe::Token) = 0;
};
// Message-thread lifecycle. No device enumeration/open/close is done by audio.
class MidiInputDeviceManager {
public:
    enum class State { off, test, connected, disconnected, error };
    MidiInputDeviceManager(MidiInputBackend& b, MidiInputPipe& p) : backend(b), pipe(p) { refresh(); }
    ~MidiInputDeviceManager() { close(); }
    void selectOff() { close(); wanted.clear(); name.clear(); state = State::off; }
    void selectTest() {
        close(); wanted.clear(); name = "Test input"; testToken = pipe.beginSession(); state = State::test;
    }
    void selectDevice(const std::string& id) {
        close(); wanted = id;
        const auto* device = find(id);
        name = device ? device->name : id;
        state = State::disconnected;
        if (device) connect();
    }
    void refresh(bool retry = false) {
        devices = backend.available();
        if (wanted.empty()) return;
        const auto* device = find(wanted);
        if (!device) {
            if (connection) close();
            state = State::disconnected;
        }
        if (device) {
            name = device->name;
            if (retry && connection) close();
            if (!connection && (state == State::disconnected || retry)) connect();
        }
    }
    bool sendTest(int status, int note, int velocity) noexcept {
        return state == State::test && pipe.push(testToken, status, note, velocity);
    }
    State getState() const noexcept { return state; }
    const std::vector<MidiInputDevice>& getDevices() const noexcept { return devices; }
    const std::string& selectedIdentifier() const noexcept { return wanted; }
    const std::string& selectedName() const noexcept { return name; }
private:
    const MidiInputDevice* find(const std::string& id) const {
        for (const auto& d : devices) if (d.identifier == id) return &d;
        return nullptr;
    }
    void close() {
        pipe.beginSession(); // Reject callbacks already in flight before stop.
        testToken = 0;
        if (connection) { connection->stop(); connection.reset(); }
    }
    void connect() {
        const auto token = pipe.beginSession();
        connection = backend.open(wanted, pipe, token);
        if (connection) { state = State::connected; connection->start(); }
        else state = State::error;
    }
    MidiInputBackend& backend;
    MidiInputPipe& pipe;
    std::unique_ptr<MidiInputConnection> connection;
    std::vector<MidiInputDevice> devices;
    std::string wanted, name;
    MidiInputPipe::Token testToken{};
    State state{State::off};
};
}
