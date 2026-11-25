// "Update.exe" (the launcher under that name): brings the folder up to the newest release of the project's repository.
//
//   https://github.com/<kRepo>/releases/latest/download/version.txt      "<number> <text>"
//   .../update.zip        every file of the folder that can change (all but the two big compiler programs)
//   .../update.zip.sig    RSA signature (PKCS#1 v1.5, SHA-256) of update.zip, made with the release key
//
// It asks for version.txt, compares its number with VERSION.txt beside this program, and when the release is newer
// downloads the two files, checks the signature against the public key below (so a file that did not come from the
// release key is never unpacked, whatever server answered), and unpacks it over the folder with Windows' own tar.
// The game then rebuilds what it must on its next start (the SDK does that when Suffering2.exe is newer than the
// built code). Nothing of the player's is touched: saves, settings, the image and the logs are not in the zip.
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>

static const wchar_t kRepoPath[] = L"/sh2coop/The-Suffering-Ties-that-Bind-CO-OP-MOD/releases/latest/download/";
static const char kPublicKey[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAxBM+O8mIMbNukfJIWhCT\n"
    "tlMRIVOtMeSQLyKlCMmapPLnOV9TNNYr9OuG07QGI2/kgnAscxZkSE//zeUrNEIo\n"
    "LNnZ57bgIUzJjH5ecyA10vDh6QMR3JQisadoT6xJirsusJr9BFk+p4SyPsjK/MS5\n"
    "DQYBgmwoTsl2nNfu1Cp4bcgH22kWWfm4uD2Z84Zhcp2UjQ7BsKtBjqrjh3dqrqxa\n"
    "bdRDXDlyIyhPIBOu0PrmJqTEtfGOOTUEsGtyESpV5u+IZ3v+PAQdApx2zOEKbQVj\n"
    "kL9H1sNuPP6gvHYyYcby9VzVwOy1qxWFgzI9d13E2EE0Eh7GexZZ3Bc/4ZzkbgOJ\n"
    "CEkG8192Sy5HafYmXP/xoC6nTwY8mw1Q+WJ7vqslmonK/MbLFycGF1BP+HRvJKe+\n"
    "aS1GM5vTm1TxGSH13UZeXOt7zYHwwQ7olccTXeQ+4Uk7/tDLX9Wlh3eDPpp+QBPP\n"
    "9KqeRHNmkWTopmFIckNomFeUAdE4JTX2hTUnirDOV0RrAgMBAAE=\n"
    "-----END PUBLIC KEY-----\n";

// GET https://github.com<kRepoPath><name> into a file (redirects followed). -> 0 ok, else an HTTP status or -1
static int Download(const wchar_t* name, const wchar_t* to, const wchar_t* what) {
    int result = -1;
    wchar_t path[512];
    _snwprintf(path, 512, L"%ls%ls", kRepoPath, name);
    HINTERNET session = WinHttpOpen(L"Suffering2-Update", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, NULL)) {
        DWORD status = 0, size = sizeof status, length = 0, length_size = sizeof length;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, NULL, &length, &length_size, NULL);
        if (status != 200) result = (int)status;
        else {
            HANDLE file = CreateFileW(to, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (file != INVALID_HANDLE_VALUE) {
                static char buffer[1 << 16];
                DWORD got = 0, written = 0, total = 0, last = 0;
                result = 0;
                while (WinHttpReadData(request, buffer, sizeof buffer, &got) && got) {
                    if (!WriteFile(file, buffer, got, &written, NULL) || written != got) { result = -1; break; }
                    total += got;
                    if (what && GetTickCount() - last > 200) {
                        wchar_t text[300];
                        if (length) _snwprintf(text, 300, L"%ls\n\n%.1f of %.1f MB", what, total / 1048576.0, length / 1048576.0);
                        else _snwprintf(text, 300, L"%ls\n\n%.1f MB", what, total / 1048576.0);
                        ShowLine(text);
                        last = GetTickCount();
                    }
                }
                if (length && total != length) result = -1;
                CloseHandle(file);
                if (result) DeleteFileW(to);
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return result;
}

static int ReadSmall(const wchar_t* path, char* out, DWORD count) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    ReadFile(f, out, count - 1, &got, NULL);
    CloseHandle(f);
    out[got] = 0;
    return (int)got;
}

// SHA-256 of a file
static int HashFile(const wchar_t* path, BYTE hash[32]) {
    int ok = 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f != INVALID_HANDLE_VALUE && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        static BYTE buffer[1 << 16];
        DWORD got = 0;
        ok = 1;
        while (ReadFile(f, buffer, sizeof buffer, &got, NULL) && got)
            if (BCryptHashData(h, buffer, got, 0) != 0) { ok = 0; break; }
        if (ok && BCryptFinishHash(h, hash, 32, 0) != 0) ok = 0;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return ok;
}

// is `signature` the release key's signature of this SHA-256?
static int SignatureGood(const BYTE hash[32], const BYTE* signature, DWORD signature_size) {
    int ok = 0;
    BYTE der[1024];
    DWORD der_size = sizeof der, info_size = 0;
    CERT_PUBLIC_KEY_INFO* info = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    if (CryptStringToBinaryA(kPublicKey, 0, CRYPT_STRING_BASE64HEADER, der, &der_size, NULL, NULL) &&
        CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, der, der_size, CRYPT_DECODE_ALLOC_FLAG, NULL, &info, &info_size) &&
        CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, info, 0, NULL, &key)) {
        BCRYPT_PKCS1_PADDING_INFO padding = {BCRYPT_SHA256_ALGORITHM};
        ok = BCryptVerifySignature(key, &padding, (PUCHAR)hash, 32, (PUCHAR)signature, signature_size, BCRYPT_PAD_PKCS1) == 0;
    }
    if (key) BCryptDestroyKey(key);
    if (info) LocalFree(info);
    return ok;
}

static int RunQuiet(wchar_t* command);

// -> the process's exit code
static int Update(void) {
    wchar_t version_file[MAX_PATH * 2], work[MAX_PATH * 2], remote_file[MAX_PATH * 2], zip[MAX_PATH * 2], sig[MAX_PATH * 2], game[MAX_PATH * 2];
    Path(version_file, MAX_PATH * 2, L"VERSION.txt");
    Path(work, MAX_PATH * 2, L"Game Files\\update");
    Path(remote_file, MAX_PATH * 2, L"Game Files\\update\\version.txt");
    Path(zip, MAX_PATH * 2, L"Game Files\\update\\update.zip");
    Path(sig, MAX_PATH * 2, L"Game Files\\update\\update.zip.sig");
    Path(game, MAX_PATH * 2, L"Game Files\\Suffering2.exe");
    CreateDirectoryW(work, NULL);
    // what an earlier update left of this program under its old name
    wchar_t old[MAX_PATH * 2];
    Path(old, MAX_PATH * 2, L"Update.old.exe");
    DeleteFileW(old);

    char mine[200] = "0", theirs[200] = "";
    ReadSmall(version_file, mine, sizeof mine);
    ShowLine(L"Looking for a newer version");
    int got = Download(L"version.txt", remote_file, NULL);
    if (got != 0 || !ReadSmall(remote_file, theirs, sizeof theirs)) {
        wchar_t text[400];
        if (got == 404) _snwprintf(text, 400, L"No release was found to update from yet.");
        else _snwprintf(text, 400, L"The update server could not be reached (%d).\n\nCheck the internet connection and try again.", got);
        Tell(text, MB_ICONINFORMATION);
        return 1;
    }
    for (char* p = theirs; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; }
    for (char* p = mine; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; }
    if (atoi(theirs) <= atoi(mine)) {
        wchar_t text[500];
        _snwprintf(text, 500, L"You have the newest version.\n\n%hs", mine);
        Tell(text, MB_ICONINFORMATION);
        return 0;
    }
    // the game must not be running: its program is replaced
    HANDLE probe = CreateFileW(game, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (probe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        Tell(L"The game is running. Close it, then start Update again.", MB_ICONINFORMATION);
        return 1;
    }
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);

    wchar_t line[400];
    _snwprintf(line, 400, L"Downloading version %hs", theirs);
    ShowLine(line);
    BYTE hash[32], signature[1024];
    DWORD signature_size = 0;
    int ok = Download(L"update.zip.sig", sig, NULL) == 0 && Download(L"update.zip", zip, line) == 0;
    if (ok) {
        HANDLE f = CreateFileW(sig, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) { ReadFile(f, signature, sizeof signature, &signature_size, NULL); CloseHandle(f); }
    }
    if (!ok) {
        Tell(L"The update could not be downloaded.\n\nCheck the internet connection and try again. Nothing was changed.", MB_ICONERROR);
        return 1;
    }
    ShowLine(L"Checking the download");
    if (!HashFile(zip, hash) || !signature_size || !SignatureGood(hash, signature, signature_size)) {
        DeleteFileW(zip);
        Tell(L"The downloaded update is not signed by this project's release key, so it was thrown away.\n\nNothing was changed. Try again later, and tell the project if it keeps happening.", MB_ICONERROR);
        return 1;
    }
    // this program is in the zip too, and a running program cannot be written over: it steps aside under another name
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(NULL, self, MAX_PATH * 2);
    MoveFileExW(self, old, MOVEFILE_REPLACE_EXISTING);
    ShowLine(L"Putting the new version in place");
    wchar_t command[MAX_PATH * 6];
    _snwprintf(command, MAX_PATH * 6, L"\"%ls\\System32\\tar.exe\" -xf \"%ls\" -C \"%ls\"", _wgetenv(L"SystemRoot") ? _wgetenv(L"SystemRoot") : L"C:\\Windows", zip, g_root);
    int code = RunQuiet(command);
    if (code != 0) {
        MoveFileExW(old, self, 0);  // back, if the zip did not bring a new one
        wchar_t text[400];
        _snwprintf(text, 400, L"The update could not be unpacked (%d).\n\nClose the game and anything that has the folder's files open, then start Update again.", code);
        Tell(text, MB_ICONERROR);
        return 1;
    }
    DeleteFileW(zip);
    DeleteFileW(sig);
    wchar_t text[600];
    _snwprintf(text, 600, L"Updated to version %hs.\n\nStart \"The Suffering 2\" as usual. The first start after an update takes a minute or two longer.", theirs);
    Tell(text, MB_ICONINFORMATION);
    return 0;
}
