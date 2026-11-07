// Normalize i386 absolute relocation numbers written by GNU objcopy for the fixture.
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input)
        return 1;
    std::string bytes(std::istreambuf_iterator<char>(input), {});
    auto read = [&](size_t offset, int width) {
        unsigned value = 0;
        for (int i = 0; i < width; ++i)
            value |= unsigned(static_cast<unsigned char>(bytes.at(offset + i))) << (i * 8);
        return value;
    };
    if (read(0, 2) != 0x14c)
        throw std::runtime_error("expected i386 COFF");
    const auto sections = read(2, 2);
    for (unsigned i = 0; i < sections; ++i) {
        const auto header = 20 + 40 * i;
        const auto table = read(header + 24, 4);
        const auto count = read(header + 32, 2);
        for (unsigned j = 0; j < count; ++j) {
            const auto offset = table + 10 * j + 8;
            const auto kind = read(offset, 2);
            if (kind != 1 && kind != 6)
                throw std::runtime_error("unexpected fixture relocation");
            bytes.at(offset) = 6;
        }
    }
    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size());
    return output.flush() ? 0 : 1;
}
