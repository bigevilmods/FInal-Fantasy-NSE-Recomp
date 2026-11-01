#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
#include <type_traits>

class StateArchive {
public:
    enum Mode { Write, Validate, Read };
    Mode mode;
    std::vector<uint8_t> &data;
    size_t position = 0;
    bool good = true;
    StateArchive(Mode m, std::vector<uint8_t> &d) : mode(m), data(d) {}
    bool reading() const { return mode == Read; }
    void bytes(void *ptr, size_t n) {
        if (!good) return;
        if (mode == Write) {
            size_t start = data.size(); data.resize(start+n);
            memcpy(data.data()+start,ptr,n);
        } else {
            if (position > data.size() || n > data.size()-position) { good=false; return; }
            if (mode == Read) memcpy(ptr,data.data()+position,n);
        }
        position += n;
    }
    template<class T> void value(T &v) {
        static_assert(std::is_trivially_copyable<T>::value,"State data must not contain pointers");
        bytes(&v,sizeof(v));
    }
};

void mem_state(StateArchive &archive);
void ppu_state(StateArchive &archive);
void apu_state(StateArchive &archive);
void hd_state(StateArchive &archive);
std::vector<uint8_t> state_capture();
bool state_restore(std::vector<uint8_t> &payload);
bool state_save_file(const char *path);
bool state_load_file(const char *path);
const char *state_error();
