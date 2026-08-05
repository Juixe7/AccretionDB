#ifndef FORGELSM_OPTIONS_H
#define FORGELSM_OPTIONS_H

#include <cstddef>

namespace forgelsm {

struct Options {
    bool sync_writes = true;
    size_t vlog_shards = 4;
    size_t flush_threshold = 0;
    size_t l0_compaction_trigger = 0;
    bool create_if_missing = true;
    bool error_if_exists = false;
    bool background_compaction = true;
    bool background_gc = true;
    bool quiet_mode = false;
    size_t l1_max_files = 0; // Legacy
    size_t max_levels = 6;
    size_t level_size_multiplier = 10;
};

} // namespace forgelsm

#endif // FORGELSM_OPTIONS_H
