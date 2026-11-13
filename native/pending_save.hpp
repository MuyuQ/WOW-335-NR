#pragma once
namespace nr_files {
struct PendingSave {
    bool dirty = false;
    bool failed = false;
    unsigned IntervalMs() const { return failed ? 1000u : 200u; }
    template<class Save> bool Try(Save save) {
        if (!save()) { failed = true; return false; }
        dirty = false;
        failed = false;
        return true;
    }
};
}
