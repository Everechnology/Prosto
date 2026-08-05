#pragma once

#include "prosto/types.hpp"
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

namespace prosto {

// String / file utilities (utils.cpp)
std::string trim(const std::string& s);
bool startsWith(const std::string& s, const std::string& p);
bool endsWith(const std::string& s, const std::string& p);
std::string upperString(const std::string& s);
std::string lowerString(const std::string& s);
std::vector<std::string> splitString(const std::string& s, const std::string& sep);
std::vector<std::string> splitLines(const std::string& s);
std::string readFileAll(const std::string& path);
void writeFileAll(const std::string& path, const std::string& text, bool append = false);
std::string generateUUID();
std::string formatDate(const std::string& fmt);
std::string platformName();
std::string fileMtimeString(const std::string& path);

// Format / JSON
std::string formatString(const std::string& fmt, const std::vector<Value>& args);
std::string templateString(const std::string& tmpl, const Value& data);
nlohmann::json valueToJson(const Value& v);
Value jsonToValue(const nlohmann::json& j);
Value getKwargsDict(std::vector<Value>& args);

// Crypto / encoding
std::string hexEncodeString(const std::string& s);
std::string hexDecodeString(const std::string& s);
std::string base64EncodeImpl(const unsigned char* data, size_t len, bool url);
std::string base64DecodeImpl(const std::string& in, bool url);
std::string urlEncodeString(const std::string& s);
std::string urlDecodeString(const std::string& s);
std::string htmlEscapeString(const std::string& s);
std::string htmlUnescapeString(const std::string& s);
uint32_t crc32String(const std::string& s);
std::string evpDigestHex(const std::string& algo, const unsigned char* data, size_t len);
std::string evpFileDigestHex(const std::string& path, const std::string& algo);
std::string hmacSha256Hex(const std::string& key, const std::string& msg);

// HTTP / filesystem / process
Value doHttpRequest(const std::string& method, const std::string& url,
                    const Value& headers, const Value& data, int timeout);
Value makeHttpResponse(long status, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body);
Value makeEFCObject(const std::string& path);
bool httpDownload(const std::string& url, const std::string& savePath, int timeout);
bool zipFolder(const std::string& path, int level, bool removeAfter);
Value searchFiles(const std::string& pattern, const Value& recursive);
std::vector<std::string> globFiles(const std::string& pattern);
int spawnProcess(const std::vector<std::string>& argv, const std::string& cwd,
                 const std::string& stdoutPath, const std::string& stderrPath);
void openFileWithDefaultApp(const std::string& path);

// Thread-local RNG accessor for builtins
std::mt19937_64& globalRng();

} // namespace prosto
