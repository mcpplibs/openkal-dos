import std;
namespace fs = std::filesystem;

int main() {
    std::println("cwd {}", fs::current_path().string());
    { std::ofstream("NOTE.TXT") << "first line\nsecond line\n"; }
    { std::ofstream("NOTE.TXT", std::ios::app) << "appended\n"; }
    std::ifstream in("NOTE.TXT");
    int n = 0;
    for (std::string line; std::getline(in, line);) std::println("read {}: {}", ++n, line);
    in.close();
    std::println("size {}", fs::file_size("NOTE.TXT"));
    fs::create_directory("SUB");
    fs::rename("NOTE.TXT", "SUB/MOVED.TXT");
    std::vector<std::string> names;
    for (auto& e : fs::directory_iterator("SUB")) names.push_back(e.path().filename().string());
    std::println("SUB holds {}", names);
    fs::resize_file("SUB/MOVED.TXT", 40);
    std::println("resized to {}", fs::file_size("SUB/MOVED.TXT"));
    fs::remove_all("SUB");
    std::println("removed: {}", !fs::exists("SUB"));
    return 9;
}
