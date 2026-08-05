#ifndef FORGELSM_VERSION_EDIT_H
#define FORGELSM_VERSION_EDIT_H

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace forgelsm {

struct FileMetaData {
    uint32_t sequence;
    uint64_t file_size;
    std::string min_key;
    std::string max_key;
};

class VersionEdit {
public:
    void add_file(int level, uint32_t sequence, uint64_t file_size, 
                  const std::string& min_key, const std::string& max_key) {
        new_files_.push_back({level, {sequence, file_size, min_key, max_key}});
    }

    void delete_file(int level, uint32_t sequence) {
        deleted_files_.push_back({level, sequence});
    }

    void set_next_file_sequence(uint32_t seq) {
        has_next_file_sequence_ = true;
        next_file_sequence_ = seq;
    }
    
    void encode_to(std::string& dst) const;
    bool decode_from(const std::string& src);

    const std::vector<std::pair<int, FileMetaData>>& new_files() const { return new_files_; }
    const std::vector<std::pair<int, uint32_t>>& deleted_files() const { return deleted_files_; }
    
    bool has_next_file_sequence() const { return has_next_file_sequence_; }
    uint32_t next_file_sequence() const { return next_file_sequence_; }

private:
    std::vector<std::pair<int, FileMetaData>> new_files_;
    std::vector<std::pair<int, uint32_t>> deleted_files_;
    
    bool has_next_file_sequence_ = false;
    uint32_t next_file_sequence_ = 0;
};

} // namespace forgelsm

#endif // FORGELSM_VERSION_EDIT_H
