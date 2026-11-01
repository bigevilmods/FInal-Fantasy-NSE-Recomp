#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <io.h>
#include "state.h"
#include "nes.h"

static char error_text[200] = {};
static uint32_t checksum(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffff;
    while (n--) {
        c ^= *p++;
        for (int b=0;b<8;++b) c=(c>>1)^(0xedb88320u & (0u-(c&1)));
    }
    return ~c;
}
static void serialize(StateArchive &a) { mem_state(a); ppu_state(a); apu_state(a); hd_state(a); }
const char *state_error() { return error_text; }
std::vector<uint8_t> state_capture() {
    std::vector<uint8_t> data;
    StateArchive a(StateArchive::Write,data); serialize(a);
    return data;
}
bool state_restore(std::vector<uint8_t> &data) {
    StateArchive check(StateArchive::Validate,data); serialize(check);
    if (!check.good || check.position != data.size()) {
        strcpy_s(error_text,"Estado incompatível com esta versão."); return false;
    }
    StateArchive load(StateArchive::Read,data); serialize(load);
    error_text[0]=0;
    return load.good;
}
struct Header {
    char magic[8];
    uint32_t version, rom_crc, size, crc;
};
static_assert(sizeof(Header)==24,"Stable state header");
bool state_save_file(const char *path) {
    auto data=state_capture();
    Header h={{'F','F','S','T','A','T','E','1'},1,
        checksum((const uint8_t *)prg,sizeof(prg)),(uint32_t)data.size(),checksum(data.data(),data.size())};
    char temporary[1024];
    if (snprintf(temporary,sizeof(temporary),"%s.tmp",path)>=(int)sizeof(temporary)) return false;
    FILE *f=fopen(temporary,"wb");
    if (!f) { strcpy_s(error_text,"Não foi possível criar o arquivo de estado."); return false; }
    bool ok=fwrite(&h,sizeof(h),1,f)==1 && fwrite(data.data(),1,data.size(),f)==data.size();
    if (ok) ok=fflush(f)==0 && _commit(_fileno(f))==0;
    if (fclose(f)!=0) ok=false;
    if (ok) ok=MoveFileExA(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if (!ok) { DeleteFileA(temporary); strcpy_s(error_text,"Falha ao gravar o estado; arquivo anterior preservado."); }
    else error_text[0]=0;
    return ok;
}
bool state_load_file(const char *path) {
    FILE *f=fopen(path,"rb");
    if (!f) { strcpy_s(error_text,"Nenhum estado salvo neste slot."); return false; }
    Header h={};
    bool ok=fread(&h,sizeof(h),1,f)==1 && !memcmp(h.magic,"FFSTATE1",8)
        && h.version==1 && h.size<=32*1024*1024;
    if (ok && h.rom_crc!=checksum((const uint8_t *)prg,sizeof(prg))) {
        fclose(f); strcpy_s(error_text,"Este estado pertence a outra ROM."); return false;
    }
    std::vector<uint8_t> data;
    if (ok) {
        data.resize(h.size);
        ok=fread(data.data(),1,data.size(),f)==data.size() && fgetc(f)==EOF
            && checksum(data.data(),data.size())==h.crc;
    }
    fclose(f);
    if (!ok) { strcpy_s(error_text,"Estado inválido ou danificado. A partida não foi alterada."); return false; }
    return state_restore(data);
}
