#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "miniz.h"

namespace Updater {
namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/OverkillLabs3/driveclub-recomp/releases/latest";
// Only files of this project's releases are downloaded, whatever the answer says.
constexpr wchar_t kAssetPrefix[] = L"https://github.com/OverkillLabs3/driveclub-recomp/releases/download/";

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::vector<int> Parts(const std::wstring& v) {
  std::vector<int> parts;
  size_t i = 0;
  while (i < v.size()) {
    size_t e = v.find(L'.', i);
    if (e == std::wstring::npos) e = v.size();
    parts.push_back(_wtoi(v.substr(i, e - i).c_str()));
    i = e + 1;
  }
  return parts;
}

// One HTTPS GET. With `file` set the body goes to that file (progress 0..100 when the size is known),
// otherwise into `body`.
bool Get(const std::wstring& url, std::string* body, const std::wstring* file, const std::function<void(int)>* progress,
         std::wstring& error) {
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256], path[2048];
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  uc.dwExtraInfoLength = DWORD(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
    error = L"Not an https address.";
    return false;
  }
  std::wstring target = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) + std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  HINTERNET session = WinHttpOpen(L"DriveClub-Recomp-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  if (!session) {
    error = L"No network access.";
    return false;
  }
  WinHttpSetTimeouts(session, 8000, 8000, 15000, 30000);
  HINTERNET conn = WinHttpConnect(session, std::wstring(uc.lpszHostName, uc.dwHostNameLength).c_str(), uc.nPort, 0);
  HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                       : nullptr;
  bool ok = false;
  HANDLE out = INVALID_HANDLE_VALUE;
  if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
    DWORD64 total = 0;
    wchar_t len[32];
    size = sizeof(len);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &size, nullptr)) total = _wtoi64(len);
    if (status != 200) {
      error = L"The server answered " + std::to_wstring(status) + L".";
    } else {
      if (file) out = CreateFileW(file->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file && out == INVALID_HANDLE_VALUE) {
        error = L"Cannot write the download.";
      } else {
        ok = true;
        DWORD64 got = 0;
        std::vector<char> buf(64 * 1024);
        for (;;) {
          DWORD n = 0;
          if (!WinHttpReadData(req, buf.data(), DWORD(buf.size()), &n)) {
            ok = false;
            error = L"The download was interrupted.";
            break;
          }
          if (n == 0) break;
          if (body) body->append(buf.data(), n);
          if (file) {
            DWORD w = 0;
            if (!WriteFile(out, buf.data(), n, &w, nullptr) || w != n) {
              ok = false;
              error = L"Cannot write the download.";
              break;
            }
          }
          got += n;
          if (progress && total) (*progress)(int((std::min<DWORD64>)(100, got * 100 / total)));
        }
      }
    }
  } else if (error.empty()) {
    error = L"Cannot reach the server.";
  }
  if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
  if (req) WinHttpCloseHandle(req);
  if (conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return ok;
}

bool Sha256(const std::wstring& path, std::string& hex) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
            BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
  std::vector<UCHAR> buf(1 << 20);
  while (ok) {
    DWORD n = 0;
    if (!ReadFile(f, buf.data(), DWORD(buf.size()), &n, nullptr)) ok = false;
    else if (n == 0) break;
    else ok = BCryptHashData(hash, buf.data(), n, 0) == 0;
  }
  UCHAR digest[32];
  ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(f);
  if (!ok) return false;
  hex.clear();
  char two[3];
  for (UCHAR b : digest) {
    std::snprintf(two, sizeof(two), "%02x", b);
    hex += two;
  }
  return true;
}

bool Run(const std::wstring& command, DWORD wait_ms, DWORD& exit_code, bool detached) {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = command;
  const DWORD flags = CREATE_NO_WINDOW | (detached ? DETACHED_PROCESS : 0);
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) return false;
  if (wait_ms) {
    WaitForSingleObject(pi.hProcess, wait_ms);
    GetExitCodeProcess(pi.hProcess, &exit_code);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

// Where the download is unpacked, next to DriveClub.exe. Players often keep the v1.28 update dump in a folder named
// "update" there, so the updater uses its own name and deletes a folder only when it holds nothing but the files
// it writes itself; 0.3.0 and 0.3.1 unpacked into "update", their leftovers go by the same rule.
constexpr wchar_t kStaging[] = L"\\update_download";
constexpr wchar_t kOldStaging[] = L"\\update";

// True when `dir` exists and holds only package.zip, the unpacked "new" folder and apply.cmd.
bool OnlyOwnFiles(const std::wstring& dir) {
  WIN32_FIND_DATAW data;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &data);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool own = true;
  do {
    const wchar_t* n = data.cFileName;
    if (wcscmp(n, L".") == 0 || wcscmp(n, L"..") == 0) continue;
    if (_wcsicmp(n, L"package.zip") != 0 && _wcsicmp(n, L"new") != 0 && _wcsicmp(n, L"apply.cmd") != 0 &&
        _wcsicmp(n, L"launcher-update.exe") != 0) own = false;
  } while (own && FindNextFileW(h, &data));
  FindClose(h);
  return own;
}

void RemoveTree(const std::wstring& dir) {
  std::wstring from = dir + L'\0';
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT;
  SHFileOperationW(&op);
}

// The unpacking and the copying are done here, not by tar.exe, robocopy and cmd.exe: those are not there under Wine and
// Proton, and a script the launcher leaves behind may not survive the launcher's own exit there.

void MakeDirs(const std::wstring& dir) {
  for (size_t i = 3; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
  }
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& data) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 31);
  if (ok) {
    data.resize(size_t(size.QuadPart));
    size_t done = 0;
    while (ok && done < data.size()) {
      DWORD n = 0;
      ok = ReadFile(f, data.data() + done, DWORD((std::min<size_t>)(data.size() - done, 1u << 24)), &n, nullptr) && n > 0;
      done += n;
    }
  }
  CloseHandle(f);
  return ok;
}

bool WriteWholeFile(const std::wstring& path, const void* data, size_t size) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  size_t done = 0;
  while (ok && done < size) {
    DWORD n = 0;
    ok = WriteFile(f, static_cast<const uint8_t*>(data) + done, DWORD((std::min<size_t>)(size - done, 1u << 24)), &n, nullptr) && n > 0;
    done += n;
  }
  return CloseHandle(f) && ok;
}

// Unpacks the zip into `dest`. A name that leaves the folder (an absolute path, a drive, ..) stops it.
bool Unpack(const std::wstring& zip_path, const std::wstring& dest, std::wstring& error) {
  std::vector<uint8_t> data;
  if (!ReadWholeFile(zip_path, data)) {
    error = L"The download could not be read.";
    return false;
  }
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
    error = L"The download is not a zip file. Nothing was changed.";
    return false;
  }
  bool ok = true;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; ok && i < count; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      error = L"The download could not be read.";
      ok = false;
      break;
    }
    std::string name = st.m_filename;
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos ||
        name.find("..") != std::string::npos) {
      error = L"The download holds a file name that is not allowed. Nothing was changed.";
      ok = false;
      break;
    }
    std::replace(name.begin(), name.end(), '/', '\\');
    const std::wstring target = dest + L"\\" + Widen(name);
    if (st.m_is_directory) {
      MakeDirs(target);
      continue;
    }
    MakeDirs(target.substr(0, target.find_last_of(L'\\')));
    size_t size = 0;
    void* bytes = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
    if (!bytes && st.m_uncomp_size != 0) {
      error = L"A file in the download could not be unpacked.";
      ok = false;
      break;
    }
    ok = WriteWholeFile(target, bytes, size);
    mz_free(bytes);
    if (!ok) error = L"A file of the update could not be written.";
  }
  mz_zip_reader_end(&zip);
  return ok;
}

bool EqualsNoCase(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

// What the update does not touch: the player's own folders and settings.
bool IsPlayersOwn(const std::wstring& relative) {
  const size_t slash = relative.find(L'\\');
  if (slash == std::wstring::npos) return EqualsNoCase(relative, L"launcher.ini");
  const std::wstring top = relative.substr(0, slash);
  for (const wchar_t* own : {L"data", L"logs", L"mods", L"update", L"update_download"}) {
    if (EqualsNoCase(top, own)) return true;
  }
  return false;
}

void ListFiles(const std::wstring& root, const std::wstring& relative, std::vector<std::wstring>& out) {
  WIN32_FIND_DATAW data;
  HANDLE h = FindFirstFileW((root + (relative.empty() ? std::wstring() : L"\\" + relative) + L"\\*").c_str(), &data);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const std::wstring n = data.cFileName;
    if (n == L"." || n == L"..") continue;
    const std::wstring rel = relative.empty() ? n : relative + L"\\" + n;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(root, rel, out);
    else out.push_back(rel);
  } while (FindNextFileW(h, &data));
  FindClose(h);
}

// A file that is in use for a moment (an antivirus scan) is tried again for a while.
bool CopyWithRetries(const std::wstring& from, const std::wstring& to) {
  for (int attempt = 0; attempt < 40; ++attempt) {
    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
    Sleep(250);
  }
  return false;
}

// Copies the new files over the program files. The launcher's own exe goes last and is first renamed aside, which a
// running program allows; the first start of the new launcher deletes it.
std::wstring RunningLauncherName() {
  wchar_t path[32768];
  const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
  if (!n || n >= 32768) return L"DriveClub.exe";
  const std::wstring full(path, n);
  return full.substr(full.find_last_of(L"\\/") + 1);
}
bool Apply(const std::wstring& root, const std::wstring& home, std::wstring& error,
           const std::wstring& running = RunningLauncherName()) {
  std::vector<std::wstring> files;
  ListFiles(root, L"", files);
  std::sort(files.begin(), files.end());
  const std::wstring self = L"DriveClub.exe";
  for (const auto& rel : files) {
    if (IsPlayersOwn(rel) || EqualsNoCase(rel, self.c_str()) || EqualsNoCase(rel, running.c_str())) continue;
    const std::wstring to = home + L"\\" + rel;
    MakeDirs(to.substr(0, to.find_last_of(L'\\')));
    if (!CopyWithRetries(root + L"\\" + rel, to)) {
      error = L"The file " + rel + L" could not be replaced. The update was stopped; start the launcher again and retry.";
      return false;
    }
  }
  // A native fallback can be running alongside its bootstrap parent. Rename
  // both executables aside, rather than trying to overwrite a mapped image.
  std::vector<std::wstring> own{self};
  if (!EqualsNoCase(running, self.c_str())) own.push_back(running);
  for (const auto& name : own) {
    if (GetFileAttributesW((root + L"\\" + name).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    const std::wstring exe = home + L"\\" + name, old_exe = exe + L".old";
    bool moved = false;
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
      moved = MoveFileExW(exe.c_str(), old_exe.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
      if (!moved) {
        error = L"The launcher could not be replaced.";
        return false;
      }
    }
    if (!CopyWithRetries(root + L"\\" + name, exe)) {
      if (moved) MoveFileExW(old_exe.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
      error = L"The new launcher could not be written. Nothing of the launcher was changed.";
      return false;
    }
  }
  return true;
}

bool StartLauncher(const std::wstring& home) {
  std::wstring cmd = L"\"" + home + L"\\DriveClub.exe\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, home.c_str(), &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

std::string Between(const std::string& s, size_t from, const char* open, char close, size_t* next) {
  const size_t b = s.find(open, from);
  if (b == std::string::npos) return {};
  const size_t start = b + std::string(open).size();
  const size_t e = s.find(close, start);
  if (e == std::string::npos) return {};
  if (next) *next = e + 1;
  return s.substr(start, e - start);
}

}  // namespace

bool IsNewer(const std::wstring& candidate, const std::wstring& current) {
  const auto a = Parts(candidate), b = Parts(current);
  for (size_t i = 0; i < (std::max)(a.size(), b.size()); ++i) {
    const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
    if (x != y) return x > y;
  }
  return false;
}

bool ParseRelease(const std::string& json, Release& out) {
  std::string tag = Between(json, 0, "\"tag_name\"", ',', nullptr);
  tag.erase(std::remove_if(tag.begin(), tag.end(), [](char c) { return c == ' ' || c == ':' || c == '"' || c == 'v'; }), tag.end());
  if (tag.empty()) return false;
  out = Release{};
  out.version = Widen(tag);
  size_t pos = 0;
  for (;;) {
    size_t next = 0;
    const std::string url = Between(json, pos, "\"browser_download_url\":\"", '"', &next);
    if (url.empty()) break;
    pos = next;
    const std::wstring w = Widen(url);
    if (w.rfind(kAssetPrefix, 0) != 0) continue;
    const auto ends = [&](const wchar_t* suffix) {
      const std::wstring s = suffix;
      return w.size() >= s.size() && w.compare(w.size() - s.size(), s.size(), s) == 0;
    };
    if (ends(L"-win64.zip") && w.find(L"/driveclub-recomp-") != std::wstring::npos) out.zip_url = w;
    if (ends(L"/SHA256SUMS.txt")) out.sums_url = w;
  }
  return !out.zip_url.empty() && !out.sums_url.empty();
}

bool CheckNewer(const std::wstring& current, Release& out, std::string& why) {
  std::string body;
  std::wstring error;
  if (!Get(std::wstring(L"https://") + kApiHost + kApiPath, &body, nullptr, nullptr, error)) {
    why = "update check: " + std::string(error.begin(), error.end());
    return false;
  }
  Release r;
  if (!ParseRelease(body, r)) {
    why = "update check: no release with a zip and a checksum file";
    return false;
  }
  if (!IsNewer(r.version, current)) {
    why = "update check: " + std::string(r.version.begin(), r.version.end()) + " is the newest";
    return false;
  }
  out = r;
  return true;
}

bool Stage(const Release& release, const std::wstring& home, const std::function<void(int)>& progress,
           std::wstring& root, std::wstring& error) {
  const std::wstring dir = home + kStaging;
  if (OnlyOwnFiles(dir)) RemoveTree(dir);
  if (!CreateDirectoryW(dir.c_str(), nullptr) && !OnlyOwnFiles(dir)) {
    error = L"The folder update_download next to DriveClub.exe holds other files. Move them away and try again.";
    return false;
  }
  const std::wstring zip = dir + L"\\package.zip";

  std::string sums;
  if (!Get(release.sums_url, &sums, nullptr, nullptr, error)) return false;
  // The line of the checksum file that names the zip: "<64 hex digits> *<name>".
  const std::wstring zip_name = release.zip_url.substr(release.zip_url.rfind(L'/') + 1);
  std::string expected;
  size_t line = 0;
  while (line < sums.size()) {
    size_t e = sums.find('\n', line);
    if (e == std::string::npos) e = sums.size();
    const std::string l = sums.substr(line, e - line);
    if (l.size() > 66 && Widen(l).find(zip_name) != std::wstring::npos) expected = l.substr(0, 64);
    line = e + 1;
  }
  if (expected.size() != 64) {
    error = L"The checksum file does not list the download.";
    return false;
  }

  if (!Get(release.zip_url, nullptr, &zip, &progress, error)) return false;
  std::string actual;
  if (!Sha256(zip, actual) || _stricmp(actual.c_str(), expected.c_str()) != 0) {
    error = L"The download does not match its checksum. Nothing was changed.";
    DeleteFileW(zip.c_str());
    return false;
  }

  const std::wstring unpacked = dir + L"\\new";
  CreateDirectoryW(unpacked.c_str(), nullptr);
  if (!Unpack(zip, unpacked, error)) return false;
  root = unpacked + L"\\" + zip_name.substr(0, zip_name.size() - 4);
  if (GetFileAttributesW((root + L"\\DriveClub.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
    error = L"The download does not hold DriveClub.exe. Nothing was changed.";
    return false;
  }
  return true;
}

bool Install(const Release& release, const std::wstring& home, const std::function<void(int)>& progress, std::wstring& error) {
  std::wstring root;
  if (!Stage(release, home, progress, root, error) || !Apply(root, home, error)) return false;
  if (!StartLauncher(home)) {
    error = L"The update is installed. Start the launcher again.";
    return false;
  }
  return true;
}

// Refuse an arbitrary PID: the waiter must be this package's WPF frontend.
HANDLE OpenShell(const std::wstring& home, DWORD pid) {
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return nullptr;
  wchar_t image[32768]; DWORD size = 32768;
  if (!QueryFullProcessImageNameW(process, 0, image, &size) ||
      _wcsicmp(std::wstring(image, size).c_str(), (home + L"\\DriveClub.Launcher.exe").c_str()) != 0) {
    CloseHandle(process); return nullptr;
  }
  return process;
}

bool Defer(const std::wstring& root, const std::wstring& home, DWORD shell_pid, std::wstring& error) {
  HANDLE shell = OpenShell(home, shell_pid);
  if (!shell) { error = L"The launcher process could not be verified. Nothing was installed."; return false; }
  CloseHandle(shell);
  wchar_t self[32768]; DWORD size = GetModuleFileNameW(nullptr, self, 32768);
  const std::wstring helper = home + kStaging + L"\\launcher-update.exe";
  if (!size || size >= 32768 || !CopyFileW(self, helper.c_str(), FALSE)) {
    error = L"The update helper could not be written. Nothing was installed."; return false;
  }
  const std::wstring package = root.substr(root.find_last_of(L"\\/") + 1);
  std::wstring command = L"\"" + helper + L"\" --complete-wpf-update " + std::to_wstring(shell_pid) + L" \"" + package + L"\"";
  STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
  if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, home.c_str(), &si, &pi)) {
    error = L"The update helper could not start. Nothing was installed."; return false;
  }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true;
}

bool InstallForShell(const Release& release, const std::wstring& home, unsigned long shell_pid,
                     const std::function<void(int)>& progress, std::wstring& error) {
  HANDLE shell = OpenShell(home, shell_pid);
  if (!shell) { error = L"The launcher process could not be verified. Nothing was installed."; return false; }
  CloseHandle(shell);
  std::wstring root;
  return Stage(release, home, progress, root, error) && Defer(root, home, shell_pid, error);
}

int RunDeferred(int argc, wchar_t** argv) {
  if (argc < 2 || wcscmp(argv[1], L"--complete-wpf-update") != 0) return -1;
  if (argc != 4) return 1;
  wchar_t* end = nullptr; unsigned long pid = wcstoul(argv[2], &end, 10);
  const std::wstring package = argv[3];
  if (!pid || !end || *end || package.rfind(L"driveclub-recomp-", 0) != 0 ||
      package.find_first_of(L"\\/:") != std::wstring::npos || package.find(L"..") != std::wstring::npos) return 1;
  wchar_t self[32768]; DWORD size = GetModuleFileNameW(nullptr, self, 32768);
  if (!size || size >= 32768) return 1;
  const std::wstring exe(self, size), suffix = L"\\update_download\\launcher-update.exe";
  if (exe.size() <= suffix.size() || _wcsicmp(exe.substr(exe.size() - suffix.size()).c_str(), suffix.c_str()) != 0) return 1;
  const std::wstring home = exe.substr(0, exe.size() - suffix.size());
  // Keep the real process handle across the wait; PID reuse cannot end it early.
  HANDLE shell = OpenShell(home, pid);
  if (!shell) {
    // A very fast shell may already have exited before the helper is scheduled.
    HANDLE other = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (other) { CloseHandle(other); return 1; }
    if (GetLastError() != ERROR_INVALID_PARAMETER) return 1;
  } else {
    const DWORD waited = WaitForSingleObject(shell, 120000); CloseHandle(shell);
    if (waited != WAIT_OBJECT_0) return 1;
  }
  std::wstring error;
  const bool installed = Apply(home + kStaging + L"\\new\\" + package, home, error);
  if (installed && StartLauncher(home)) return 0;
  if (error.empty()) error = L"The update is installed. Start the launcher again.";
  MessageBoxW(nullptr, error.c_str(), L"DRIVECLUB update", MB_OK | MB_ICONERROR);
  return 1;
}

void CleanUp(const std::wstring& home) {
  DeleteFileW((home + L"\\DriveClub.exe.old").c_str());
  DeleteFileW((home + L"\\" + RunningLauncherName() + L".old").c_str());
  for (const wchar_t* name : {kStaging, kOldStaging}) {
    const std::wstring dir = home + name;
    if (OnlyOwnFiles(dir)) RemoveTree(dir);
  }
}

}  // namespace Updater
