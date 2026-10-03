#pragma once

// Dvar writers synchronize with the simulation tick's read lifetime. The
// recursive scope also handles native nested setters without a lock upgrade.
void KHZ_DvarEnter();
void KHZ_DvarLeave();
void KHZ_ControlsErrorUnwind();
class KHZ_DvarScope {
public:
    KHZ_DvarScope() { KHZ_DvarEnter(); }
    ~KHZ_DvarScope() { KHZ_DvarLeave(); }
    KHZ_DvarScope(const KHZ_DvarScope&) = delete;
};
void KHZ_CommandControlBegin();
void KHZ_CommandControlEnd();
class KHZ_CommandControlScope {
public:
    explicit KHZ_CommandControlScope(bool enabled = true) : enabled_(enabled) { if (enabled_) KHZ_CommandControlBegin(); }
    ~KHZ_CommandControlScope() { if (enabled_) KHZ_CommandControlEnd(); }
    KHZ_CommandControlScope(const KHZ_CommandControlScope&) = delete;
private:
    bool enabled_;
};
