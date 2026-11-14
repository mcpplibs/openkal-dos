// okdos: the seal of a DOS program (openkal-dos; technical experiment only).
//
//   okdos seal <file>     the ELF32 i386 executable <file> becomes a DOS program: the stub
//                         (stub/stub.S) followed by the executable, written in its place
//   okdos check <file>    whether <file> is a sealed program, and of which stub
//
// The stub loads the executable's loadable segments from its own file into DPMI memory and
// enters it (src/dos.h states the hand-over). Sealing a sealed program is refused, naming it.
#include "stub_bytes.h"
import std;

namespace {

int usage() {
    std::println(std::cerr, "usage: okdos seal <file>\n       okdos check <file>");
    return 2;
}

std::optional<std::string> read(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::uint32_t u32(const std::string& b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b[at + i]);
    return v;
}
std::uint16_t u16(const std::string& b, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(b[at]) | static_cast<unsigned char>(b[at + 1]) << 8);
}

bool sealed(const std::string& b) {
    return b.size() >= sizeof kOkdosStub
        && std::equal(std::begin(kOkdosStub), std::end(kOkdosStub), b.begin(),
                      [](unsigned char s, char c) { return s == static_cast<unsigned char>(c); });
}

// What the stub requires of the executable: ELF32, little-endian, i386, an executable (not
// position-independent: the stub applies no relocation), at most 16 program headers.
std::optional<std::string> refuse(const std::string& b) {
    if (b.size() < 52 || b.compare(0, 4, "\x7f" "ELF") != 0) return "not ELF";
    if (b[4] != 1 || b[5] != 1) return "not 32-bit little-endian ELF";
    if (u16(b, 18) != 3) return "not i386 (EM_386)";
    if (u16(b, 16) != 2) return "not an executable linked at its address (ET_EXEC): the stub applies no relocation";
    if (u16(b, 44) > 16) return "more than 16 program headers";
    const std::size_t phoff = u32(b, 28);
    for (std::size_t i = 0; i < u16(b, 44); ++i) {
        const std::size_t at = phoff + i * 32;
        if (at + 32 > b.size()) return "program headers beyond the file";
        if (u32(b, at) == 1 && u32(b, at + 4) + static_cast<std::size_t>(u32(b, at + 16)) > b.size())
            return "a loadable segment beyond the file";
    }
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return usage();
    const std::string_view verb = argv[1];
    const std::filesystem::path file = argv[2];
    const auto bytes = read(file);
    if (!bytes) {
        std::println(std::cerr, "okdos: {} cannot be read", file.string());
        return 1;
    }
    if (verb == "check") {
        if (!sealed(*bytes)) {
            std::println(std::cerr, "okdos: {} is not sealed by this stub", file.string());
            return 1;
        }
        const std::string elf = bytes->substr(sizeof kOkdosStub);
        if (auto why = refuse(elf)) {
            std::println(std::cerr, "okdos: {}: the executable after the stub is {}", file.string(), *why);
            return 1;
        }
        std::println("{}: a DOS program: the okdos stub ({} bytes) and an ELF32 i386 executable ({} bytes)",
                     file.string(), sizeof kOkdosStub, elf.size());
        return 0;
    }
    if (verb != "seal") return usage();
    if (sealed(*bytes)) {
        std::println(std::cerr, "okdos: {} is sealed already", file.string());
        return 1;
    }
    if (auto why = refuse(*bytes)) {
        std::println(std::cerr, "okdos: {} cannot be sealed: {}", file.string(), *why);
        return 1;
    }
    const auto temp = std::filesystem::path(file).concat(".okdos");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(kOkdosStub), sizeof kOkdosStub);
        out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        if (!out) {
            std::println(std::cerr, "okdos: {} cannot be written", temp.string());
            return 1;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::println(std::cerr, "okdos: {} cannot replace {}: {}", temp.string(), file.string(), ec.message());
        return 1;
    }
    return 0;
}
