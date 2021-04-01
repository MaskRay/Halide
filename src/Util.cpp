#include "Util.h"
#include "Debug.h"
#include "Error.h"
#include "Introspection.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

#ifdef _MSC_VER
#include <io.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __linux__
#define CAN_GET_RUNNING_PROGRAM_NAME
#include <linux/limits.h>  // For PATH_MAX
#endif
#if defined(_MSC_VER) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#ifdef _WIN32
#include <Objbase.h>  // needed for CoCreateGuid
#include <Shlobj.h>   // needed for SHGetFolderPath
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#define CAN_GET_RUNNING_PROGRAM_NAME
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
namespace {

std::string from_utf16(LPCWSTR pStr) {
    int len = wcslen(pStr);

    int mblen = WideCharToMultiByte(CP_UTF8, 0, pStr, len, nullptr, 0, nullptr, nullptr);
    internal_assert(mblen) << "WideCharToMultiByte() failed; error " << GetLastError() << "\n";

    std::string str(mblen, 0);

    mblen = WideCharToMultiByte(CP_UTF8, 0, pStr, len, &str[0], (int)str.size(), nullptr, nullptr);
    internal_assert(mblen) << "WideCharToMultiByte() failed; error " << GetLastError() << "\n";

    return str;
}

std::wstring from_utf8(const std::string &str) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    internal_assert(wlen) << "MultiByteToWideChar() failed; error " << GetLastError() << "\n";

    std::wstring wstr(wlen, 0);

    wlen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], (int)wstr.size());
    internal_assert(wlen) << "MultiByteToWideChar() failed; error " << GetLastError() << "\n";

    return wstr;
}

}  // namespace
#endif

namespace Halide {
namespace Internal {

using std::ostringstream;
using std::string;
using std::vector;

std::string get_env_variable(char const *env_var_name) {
    if (!env_var_name) {
        return "";
    }

#ifdef _MSC_VER
    // call getenv_s without a buffer to determine the correct string length:
    size_t length = 0;
    if ((getenv_s(&length, nullptr, 0, env_var_name) != 0) || (length == 0)) {
        return "";
    }
    // call it again to retrieve the value of the environment variable;
    // note that 'length' already accounts for the null-terminator
    std::string lvl(length - 1, '@');
    size_t read = 0;
    if ((getenv_s(&read, &lvl[0], length, env_var_name) != 0) || (read != length)) {
        return "";
    }
    return lvl;
#else
    char *lvl = getenv(env_var_name);
    if (lvl) {
        return std::string(lvl);
    }
#endif

    return "";
}

string running_program_name() {
#ifndef CAN_GET_RUNNING_PROGRAM_NAME
    return "";
#else
    string program_name;
    char path[PATH_MAX] = {0};
    uint32_t size = sizeof(path);
#if defined(__linux__)
    ssize_t len = ::readlink("/proc/self/exe", path, size - 1);
#elif defined(__APPLE__)
    ssize_t len = ::_NSGetExecutablePath(path, &size);
#endif
    if (len != -1) {
#if defined(__linux__)
        path[len] = '\0';
#endif
        string tmp = std::string(path);
        program_name = tmp.substr(tmp.find_last_of('/') + 1);
    } else {
        return "";
    }
    return program_name;
#endif
}

namespace {
// We use 64K of memory to store unique counters for the purpose of
// making names unique. Using less memory increases the likelihood of
// hash collisions. This wouldn't break anything, but makes stmts
// slightly confusing to read because names that are actually unique
// will get suffixes that falsely hint that they are not.

const int num_unique_name_counters = (1 << 14);

// We want to init these to zero, but cannot use = {0} because that
// would invoke a (deleted) copy ctor. The default initialization for
// atomics doesn't guarantee any actual initialization. Fortunately
// this is a global, which is always zero-initialized.
std::atomic<int> unique_name_counters[num_unique_name_counters] = {};

int unique_count(size_t h) {
    h = h & (num_unique_name_counters - 1);
    return unique_name_counters[h]++;
}
}  // namespace

// There are three possible families of names returned by the methods below:
// 1) char pattern: (char that isn't '$') + number (e.g. v234)
// 2) string pattern: (string without '$') + '$' + number (e.g. fr#nk82$42)
// 3) a string that does not match the patterns above
// There are no collisions within each family, due to the unique_count
// done above, and there can be no collisions across families by
// construction.

string unique_name(char prefix) {
    if (prefix == '$') {
        prefix = '_';
    }
    return prefix + std::to_string(unique_count((size_t)(prefix)));
}

string unique_name(const std::string &prefix) {
    string sanitized = prefix;

    // Does the input string look like something returned from unique_name(char)?
    bool matches_char_pattern = true;

    // Does the input string look like something returned from unique_name(string)?
    bool matches_string_pattern = true;

    // Rewrite '$' to '_'. This is a many-to-one mapping, but that's
    // OK, we're about to hash anyway. It just means that some names
    // will share the same counter.
    int num_dollars = 0;
    for (size_t i = 0; i < sanitized.size(); i++) {
        if (sanitized[i] == '$') {
            num_dollars++;
            sanitized[i] = '_';
        }
        if (i > 0 && !isdigit(sanitized[i])) {
            // Found a non-digit after the first char
            matches_char_pattern = false;
            if (num_dollars) {
                // Found a non-digit after a '$'
                matches_string_pattern = false;
            }
        }
    }
    matches_string_pattern &= num_dollars == 1;
    matches_char_pattern &= prefix.size() > 1;

    // Then add a suffix that's globally unique relative to the hash
    // of the sanitized name.
    int count = unique_count(std::hash<std::string>()(sanitized));
    if (count == 0) {
        // We can return the name as-is if there's no risk of it
        // looking like something unique_name has ever returned in the
        // past or will ever return in the future.
        if (!matches_char_pattern && !matches_string_pattern) {
            return prefix;
        }
    }

    return sanitized + "$" + std::to_string(count);
}

bool starts_with(const string &str, const string &prefix) {
    if (str.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); i++) {
        if (str[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

bool ends_with(const string &str, const string &suffix) {
    if (str.size() < suffix.size()) {
        return false;
    }
    size_t off = str.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); i++) {
        if (str[off + i] != suffix[i]) {
            return false;
        }
    }
    return true;
}

string replace_all(const string &str, const string &find, const string &replace) {
    size_t pos = 0;
    string result = str;
    while ((pos = result.find(find, pos)) != string::npos) {
        result.replace(pos, find.length(), replace);
        pos += replace.length();
    }
    return result;
}

string make_entity_name(void *stack_ptr, const string &type, char prefix) {
    string name = Introspection::get_variable_name(stack_ptr, type);

    if (name.empty()) {
        return unique_name(prefix);
    } else {
        // Halide names may not contain '.'
        for (size_t i = 0; i < name.size(); i++) {
            if (name[i] == '.') {
                name[i] = ':';
            }
        }
        return unique_name(name);
    }
}

std::vector<std::string> split_string(const std::string &source, const std::string &delim) {
    std::vector<std::string> elements;
    size_t start = 0;
    size_t found = 0;
    while ((found = source.find(delim, start)) != std::string::npos) {
        elements.push_back(source.substr(start, found - start));
        start = found + delim.size();
    }

    // If start is exactly source.size(), the last thing in source is a
    // delimiter, in which case we want to add an empty string to elements.
    if (start <= source.size()) {
        elements.push_back(source.substr(start, std::string::npos));
    }
    return elements;
}

std::string extract_namespaces(const std::string &name, std::vector<std::string> &namespaces) {
    namespaces = split_string(name, "::");
    std::string result = namespaces.back();
    namespaces.pop_back();
    return result;
}

std::string extract_namespaces(const std::string &name) {
    std::vector<std::string> unused;
    return extract_namespaces(name, unused);
}

bool file_exists(const std::string &name) {
#ifdef _MSC_VER
    return _access(name.c_str(), 0) == 0;
#else
    return ::access(name.c_str(), F_OK) == 0;
#endif
}

void assert_file_exists(const std::string &name) {
    internal_assert(file_exists(name)) << "File not found: " << name;
}

void assert_no_file_exists(const std::string &name) {
    internal_assert(!file_exists(name)) << "File (wrongly) found: " << name;
}

void file_unlink(const std::string &name) {
#ifdef _MSC_VER
    _unlink(name.c_str());
#else
    ::unlink(name.c_str());
#endif
}

void ensure_no_file_exists(const std::string &name) {
    if (file_exists(name)) {
        file_unlink(name);
    }
    assert_no_file_exists(name);
}

void dir_rmdir(const std::string &name) {
#ifdef _MSC_VER
    std::wstring wname = from_utf8(name);
    internal_assert(RemoveDirectoryW(wname.c_str()))
        << "RemoveDirectoryW() failed to remove " << name << "; error " << GetLastError() << "\n";
#else
    int r = ::rmdir(name.c_str());
    internal_assert(r == 0) << "Unable to remove dir: " << name << "\n";
#endif
}

FileStat file_stat(const std::string &name) {
#ifdef _MSC_VER
    struct _stat a;
    if (_stat(name.c_str(), &a) != 0) {
        user_error << "Could not stat " << name << "\n";
    }
#else
    struct stat a;
    if (::stat(name.c_str(), &a) != 0) {
        user_error << "Could not stat " << name << "\n";
    }
#endif
    return {static_cast<uint64_t>(a.st_size),
            static_cast<uint32_t>(a.st_mtime),
            static_cast<uint32_t>(a.st_uid),
            static_cast<uint32_t>(a.st_gid),
            static_cast<uint32_t>(a.st_mode)};
}

#ifdef _WIN32
namespace {

// GetTempPath() will fail rudely if env vars aren't set properly,
// which is the case when we run under a tool in Bazel. Instead,
// look for the current user's AppData/Local/Temp path, which
// should be valid and writable in all versions of Windows that
// we support for compilation purposes.
std::string get_windows_tmp_dir() {
    // Allow overriding of the tmpdir on Windows via an env var;
    // some Windows configs can (apparently) lock down AppData/Local/Temp
    // via policy, making various things break. (Note that this is intended
    // to be a short-lived workaround; we would prefer to be able to avoid
    // requiring this sort of band-aid if possible.)
    std::string tmp_dir = get_env_variable("HL_WINDOWS_TMP_DIR");
    if (!tmp_dir.empty()) {
        return tmp_dir;
    }

    PWSTR wlocal_path;
    HRESULT ret = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wlocal_path);
    internal_assert(ret == S_OK) << "Unable to get Local AppData folder; error " << GetLastError() << "\n";

    std::string tmp = from_utf16(wlocal_path);
    CoTaskMemFree(wlocal_path);

    tmp = replace_all(tmp, "\\", "/");
    if (tmp.back() != '/') tmp += '/';
    tmp += "Temp/";
    return tmp;
}

}  //  namespace
#endif

std::string file_make_temp(const std::string &prefix, const std::string &suffix) {
    internal_assert(prefix.find('/') == string::npos &&
                    prefix.find('\\') == string::npos &&
                    suffix.find('/') == string::npos &&
                    suffix.find('\\') == string::npos);
#ifdef _WIN32
    // Windows implementations of mkstemp() try to create the file in the root
    // directory Unfortunately, that requires ADMIN privileges, which are not
    // guaranteed here.
    std::wstring tmp_dir = from_utf8(get_windows_tmp_dir());
    std::wstring wprefix = from_utf8(prefix);

    WCHAR tmp_file[MAX_PATH];
    // Note that GetTempFileNameW() actually creates the file.
    DWORD ret = GetTempFileNameW(tmp_dir.c_str(), wprefix.c_str(), 0, tmp_file);
    internal_assert(ret != 0) << "GetTempFileNameW() failed; error " << GetLastError() << "\n";
    return from_utf16(tmp_file);
#else
    std::string templ = "/tmp/" + prefix + "XXXXXX" + suffix;
    // Copy into a temporary buffer, since mkstemp modifies the buffer in place.
    std::vector<char> buf(templ.size() + 1);
    strcpy(&buf[0], templ.c_str());
    int fd = mkstemps(&buf[0], suffix.size());
    internal_assert(fd != -1) << "Unable to create temp file for (" << &buf[0] << ")\n";
    close(fd);
    return std::string(&buf[0]);
#endif
}

std::string dir_make_temp() {
#ifdef _WIN32
    std::string tmp_dir = get_windows_tmp_dir();
    // There's no direct API to do this in Windows;
    // our clunky-but-adequate approach here is to use
    // CoCreateGuid() to create a probably-unique name.
    // Add a limit on the number of tries just in case.
    for (int tries = 0; tries < 100; ++tries) {
        GUID guid;
        HRESULT hr = CoCreateGuid(&guid);
        internal_assert(hr == S_OK);
        std::ostringstream name;
        name << std::hex
             << std::setfill('0')
             << std::setw(8)
             << guid.Data1
             << std::setw(4)
             << guid.Data2
             << guid.Data3
             << std::setw(2);
        for (int i = 0; i < 8; i++) {
            name << (int)guid.Data4[i];
        }
        std::string dir = tmp_dir + name.str();
        std::wstring wdir = from_utf8(dir);
        BOOL success = CreateDirectoryW(wdir.c_str(), nullptr);
        if (success) {
            debug(1) << "temp dir is: " << dir << "\n";
            return dir;
        }
        // If name already existed, just loop and try again.
        // Any other error, break from loop and fail.
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    internal_error << "Unable to create temp directory in " << tmp_dir << "\n";
    return "";
#else
    std::string templ = "/tmp/XXXXXX";
    // Copy into a temporary buffer, since mkdtemp modifies the buffer in place.
    std::vector<char> buf(templ.size() + 1);
    strcpy(&buf[0], templ.c_str());
    char *result = mkdtemp(&buf[0]);
    internal_assert(result != nullptr) << "Unable to create temp directory.\n";
    return std::string(result);
#endif
}

std::vector<char> read_entire_file(const std::string &pathname) {
    std::ifstream f(pathname, std::ios::in | std::ios::binary);
    std::vector<char> result;

    f.seekg(0, std::ifstream::end);
    size_t size = f.tellg();
    result.resize(size);
    f.seekg(0, std::ifstream::beg);
    f.read(result.data(), result.size());
    internal_assert(f.good()) << "Unable to read file: " << pathname;
    f.close();
    return result;
}

void write_entire_file(const std::string &pathname, const void *source, size_t source_len) {
    std::ofstream f(pathname, std::ios::out | std::ios::binary);

    f.write(reinterpret_cast<const char *>(source), source_len);
    f.flush();
    internal_assert(f.good()) << "Unable to write file: " << pathname;
    f.close();
}

bool add_would_overflow(int bits, int64_t a, int64_t b) {
    int64_t max_val = 0x7fffffffffffffffLL >> (64 - bits);
    int64_t min_val = -max_val - 1;
    return ((b > 0 && a > max_val - b) ||  // (a + b) > max_val, rewritten to avoid overflow
            (b < 0 && a < min_val - b));   // (a + b) < min_val, rewritten to avoid overflow
}

bool sub_would_overflow(int bits, int64_t a, int64_t b) {
    int64_t max_val = 0x7fffffffffffffffLL >> (64 - bits);
    int64_t min_val = -max_val - 1;
    return ((b < 0 && a > max_val + b) ||  // (a - b) > max_val, rewritten to avoid overflow
            (b > 0 && a < min_val + b));   // (a - b) < min_val, rewritten to avoid overflow
}

bool mul_would_overflow(int bits, int64_t a, int64_t b) {
    int64_t max_val = 0x7fffffffffffffffLL >> (64 - bits);
    int64_t min_val = -max_val - 1;
    if (a == 0) {
        return false;
    } else if (a == -1) {
        return b == min_val;
    } else {
        // Do the multiplication as a uint64, for which overflow is
        // well defined, then cast the bits back to int64 to get
        // multiplication modulo 2^64.
        int64_t ab = (int64_t)((uint64_t)a) * ((uint64_t)b);
        // The first two clauses catch overflow mod 2^bits, assuming
        // no 64-bit overflow occurs, and the third clause catches
        // 64-bit overflow.
        return ab < min_val || ab > max_val || (ab / a != b);
    }
}

struct TickStackEntry {
    std::chrono::time_point<std::chrono::high_resolution_clock> time;
    string file;
    int line;
};

vector<TickStackEntry> tick_stack;

void halide_tic_impl(const char *file, int line) {
    string f = file;
    f = split_string(f, "/").back();
    tick_stack.push_back({std::chrono::high_resolution_clock::now(), f, line});
}

void halide_toc_impl(const char *file, int line) {
    auto t1 = tick_stack.back();
    auto t2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = t2 - t1.time;
    tick_stack.pop_back();
    for (size_t i = 0; i < tick_stack.size(); i++) {
        debug(1) << "  ";
    }
    string f = file;
    f = split_string(f, "/").back();
    debug(1) << t1.file << ":" << t1.line << " ... " << f << ":" << line << " : " << diff.count() * 1000 << " ms\n";
}

std::string c_print_name(const std::string &name) {
    ostringstream oss;

    // Prefix an underscore to avoid reserved words (e.g. a variable named "while")
    if (isalpha(name[0])) {
        oss << "_";
    }

    for (size_t i = 0; i < name.size(); i++) {
        if (name[i] == '.') {
            oss << "_";
        } else if (name[i] == '$') {
            oss << "__";
        } else if (name[i] != '_' && !isalnum(name[i])) {
            oss << "___";
        } else {
            oss << name[i];
        }
    }
    return oss.str();
}

int get_llvm_version() {
    static_assert(LLVM_VERSION > 0, "LLVM_VERSION is not defined");
    return LLVM_VERSION;
}

#ifdef _WIN32

namespace {

struct DeferredFunction {
    const std::function<void()> *run;
    LPVOID fiber;
};

void generic_fiber_entry_point(void *argument) {
    auto *action = reinterpret_cast<DeferredFunction *>(argument);
    (*action->run)();
    SwitchToFiber(action->fiber);
}

}  // namespace

#endif

void run_with_large_stack(const std::function<void()> &action) {
#if _WIN32
    SIZE_T required_stack = 8 * 1024 * 1024;

    ULONG_PTR stack_low, stack_high;
    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    ptrdiff_t stack_remaining = (char *)&stack_high - (char *)stack_low;

    if (stack_remaining < required_stack) {
        debug(1) << "Insufficient stack space (" << stack_remaining << " bytes). Switching to fiber with " << required_stack << "-byte stack.\n";

        auto was_a_fiber = IsThreadAFiber();

        auto *main_fiber = was_a_fiber ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
        internal_assert(main_fiber) << "ConvertThreadToFiber failed with code: " << GetLastError() << "\n";

        DeferredFunction func{&action, main_fiber};
        auto *lower_fiber = CreateFiber(required_stack, generic_fiber_entry_point, &func);
        internal_assert(lower_fiber) << "CreateFiber failed with code: " << GetLastError() << "\n";

        SwitchToFiber(lower_fiber);
        DeleteFiber(lower_fiber);

        debug(1) << "Returned from fiber.\n";

        if (!was_a_fiber) {
            BOOL success = ConvertFiberToThread();
            internal_assert(success) << "ConvertFiberToThread failed with code: " << GetLastError() << "\n";
        }

        return;
    }

#endif

    action();
}

}  // namespace Internal

void load_plugin(const std::string &lib_name) {
#ifdef _WIN32
    std::string lib_path = lib_name;
    if (lib_path.find('.') == std::string::npos) {
        lib_path += ".dll";
    }

    std::wstring wide_lib = from_utf8(lib_path);
    HMODULE library = LoadLibraryW(wide_lib.c_str());
    if (!library) {
        DWORD error = GetLastError();
        LPWSTR message = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);

        user_assert(message)
            << "Failed to load: " << lib_path << ".\n"
            << "FormatMessage failed while processing error in LoadLibraryW (errno "
            << error << ").\n";

        std::string err_msg = from_utf16(message);
        LocalFree(message);
        user_error << "Failed to load: " << lib_path << ";\n"
                   << "LoadLibraryW failed with error " << error << ": "
                   << err_msg << "\n";
    }
#else
    std::string lib_path = lib_name;
    if (lib_path.find('.') == std::string::npos) {
        lib_path = "lib" + lib_path + ".so";
    }
    if (dlopen(lib_path.c_str(), RTLD_LAZY) == nullptr) {
        user_error << "Failed to load: " << lib_path << ": " << dlerror() << "\n";
    }
#endif
}

}  // namespace Halide
