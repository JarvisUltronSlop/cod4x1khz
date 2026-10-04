#pragma once

// Dvar writers synchronize with the simulation tick's read lifetime. The
// recursive scope also handles native nested setters without a lock upgrade.
void KHZ_DvarEnter();
void KHZ_DvarLeave();
void KHZ_ControlsErrorUnwind();
void KHZ_DvarLocksErrorUnwind();
unsigned KHZ_DvarLockDepth();
class KHZ_DvarScope {
public:
    KHZ_DvarScope() { KHZ_DvarEnter(); }
    ~KHZ_DvarScope() { KHZ_DvarLeave(); }
    KHZ_DvarScope(const KHZ_DvarScope&) = delete;
};
// Name/hash metadata has a separate reader/writer lifetime. Looking up an
// immutable dvar record does not wait for the whole prediction tick.
void KHZ_DvarLookupEnter();
void KHZ_DvarLookupLeave();
void KHZ_DvarRegistryEnter();
void KHZ_DvarRegistryLeave();
class KHZ_DvarLookupScope {
public:
    KHZ_DvarLookupScope() { KHZ_DvarLookupEnter(); }
    ~KHZ_DvarLookupScope() { KHZ_DvarLookupLeave(); }
    KHZ_DvarLookupScope(const KHZ_DvarLookupScope&) = delete;
};
class KHZ_DvarRegistryScope {
public:
    KHZ_DvarRegistryScope() { KHZ_DvarRegistryEnter(); }
    ~KHZ_DvarRegistryScope() { KHZ_DvarRegistryLeave(); }
    KHZ_DvarRegistryScope(const KHZ_DvarRegistryScope&) = delete;
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
