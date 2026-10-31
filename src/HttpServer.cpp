#include "HttpServer.hpp"
#include "App.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace {
using nlohmann::json;
struct Socket { int fd; ~Socket() { if (fd >= 0) close(fd); } };
std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first,value.find_last_not_of(" \t")-first+1);
}
std::string Lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return std::tolower(c);});
    return value;
}
bool LoopbackHost(const std::string& authority) {
    std::string host, port;
    if (authority.starts_with("[::1]")) {
        host = "[::1]";
        if (authority.size() > 5) {
            if (authority[5] != ':') return false;
            port = authority.substr(6);
            if (port.empty()) return false;
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0,colon);
        if (colon != std::string::npos) {
            port = authority.substr(colon+1);
            if (port.empty()) return false;
        }
    }
    if (host != "localhost" && host != "127.0.0.1" && host != "[::1]") return false;
    if (!port.empty()) {
        unsigned value{};
        const auto parsed = std::from_chars(port.data(),port.data()+port.size(),value);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data()+port.size() || !value || value > 65535) return false;
    }
    return true;
}
// A timeout on each recv alone permits an indefinitely slow request. Bound the
// whole request, including partial headers and bodies, by one absolute deadline.
ssize_t Receive(int fd,char* data,size_t size,std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline-std::chrono::steady_clock::now()).count();
        if (left <= 0) return -1;
        timeval timeout{static_cast<time_t>(left/1000000),static_cast<suseconds_t>(left%1000000)};
        if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))) return -1;
        const auto n = recv(fd,data,size,0);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}
bool Send(int fd, const char* data, size_t length) {
    while (length) {
        const auto n = send(fd, data, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n; length -= n;
    }
    return true;
}
bool Header(int fd, int code, std::string_view type, uint64_t size, bool attachment = false) {
    const auto text = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK\r\n" : " Error\r\n") +
        "Content-Type: " + std::string(type) + "\r\nContent-Length: " + std::to_string(size) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n" +
        "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'\r\n" +
        (attachment ? "Content-Disposition: attachment\r\n" : "") + "\r\n";
    return Send(fd, text.data(), text.size());
}
void Reply(int fd, int code, const json& object) {
    const auto text = object.dump(-1, ' ', false, json::error_handler_t::replace);
    if (Header(fd, code, "application/json; charset=utf-8", text.size())) Send(fd, text.data(), text.size());
}
std::string Decode(const std::string& value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%') {
            if (i + 2 >= value.size()) throw std::runtime_error("Invalid encoding");
            unsigned n{};
            const auto result = std::from_chars(value.data() + i + 1, value.data() + i + 3, n, 16);
            if (result.ec != std::errc{} || result.ptr != value.data() + i + 3 || !n) throw std::runtime_error("Invalid encoding");
            out += static_cast<char>(n); i += 2;
        } else out += value[i] == '+' ? ' ' : value[i];
    }
    return out;
}
void File(int fd, const std::string& path, const std::string& type, bool attachment) {
    Socket file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat st{};
    if (file.fd < 0 || fstat(file.fd, &st) || !S_ISREG(st.st_mode)) { Reply(fd, 404, {{"error","File not found"}}); return; }
    if (!Header(fd, 200, type, st.st_size, attachment)) return;
    off_t offset = 0;
    while (offset < st.st_size) {
        const auto sent = sendfile(fd, file.fd, &offset, std::min<off_t>(1024 * 1024, st.st_size - offset));
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) break;
    }
}
void Handle(int fd, const std::string& webRoot) {
    timeval timeout{30,0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
    std::string request;
    std::array<char,4096> buffer{};
    size_t boundary;
    while ((boundary = request.find("\r\n\r\n")) == std::string::npos) {
        const auto n = Receive(fd, buffer.data(), buffer.size(), deadline);
        if (n <= 0) return;
        request.append(buffer.data(), n);
        if (request.size() > 16384) { Reply(fd, 431, {{"error","Header too large"}}); return; }
    }
    std::istringstream stream(request.substr(0, boundary));
    std::string method, target, version, line;
    if (!(stream >> method >> target >> version) || (version != "HTTP/1.1" && version != "HTTP/1.0") || !target.starts_with('/')) {
        Reply(fd,400,{{"error","Invalid request line"}}); return;
    }
    std::getline(stream, line);
    if (line != "\r" && !line.empty()) { Reply(fd,400,{{"error","Invalid request line"}}); return; }
    std::map<std::string,std::string> headers;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0 || line.front() == ' ' || line.front() == '\t') throw std::runtime_error("Invalid header");
        auto key = Lower(line.substr(0, colon));
        if (!std::all_of(key.begin(),key.end(),[](unsigned char c){return std::isalnum(c) || c == '-';})) throw std::runtime_error("Invalid header name");
        auto value = Trim(line.substr(colon + 1));
        if (!headers.emplace(key,value).second) throw std::runtime_error("Duplicate header");
    }
    const auto host = Lower(headers["host"]);
    if (!LoopbackHost(host)) { Reply(fd,403,{{"error","Expected a loopback Host"}}); return; }
    // Check browser Origin against the incoming Host, not the Android listen
    // port: ADB may forward a different computer port. The Host allowlist also
    // prevents DNS rebinding to this loopback service.
    if (method == "POST" && headers.contains("origin") && Lower(headers["origin"]) != "http://"+host) {
        Reply(fd,403,{{"error","Cross-origin requests are not allowed"}}); return;
    }
    const auto contentType = Lower(Trim(headers["content-type"].substr(0,headers["content-type"].find(';'))));
    if (method == "POST" && (headers["x-umt-client"] != "1" || contentType != "application/json")) {
        Reply(fd, 403, {{"error","Expected JSON and X-UMT-Client: 1"}}); return;
    }
    if (headers.contains("transfer-encoding")) { Reply(fd,400,{{"error","Chunked requests are not supported"}}); return; }
    size_t length = 0;
    if (headers.contains("content-length")) {
        const auto& raw = headers.at("content-length");
        auto parse = std::from_chars(raw.data(),raw.data()+raw.size(),length);
        if (parse.ec != std::errc{} || parse.ptr != raw.data()+raw.size() || length > 65536) { Reply(fd,413,{{"error","Invalid body length"}}); return; }
    }
    const auto bodyStart = boundary + 4;
    while (request.size() - bodyStart < length) {
        const auto n = Receive(fd,buffer.data(),std::min(buffer.size(),length-(request.size()-bodyStart)),deadline);
        if (n <= 0) return;
        request.append(buffer.data(),n);
    }
    if (method == "POST" && target == "/api/action") {
        const auto action = App::Action(json::parse(request.substr(bodyStart,length)));
        Reply(fd, action.value("accepted",false) ? 200 : 409, action); return;
    }
    if (method == "POST" && target == "/api/explorer") {
        const auto result = App::Explore(json::parse(request.substr(bodyStart,length)));
        Reply(fd,result.contains("error") ? 409 : 200,result); return;
    }
    if (method != "GET") { Reply(fd,405,{{"error","Method not supported"}}); return; }
    if (target == "/api/health") Reply(fd,200,{{"version","1.0.1"},{"uid",getuid()},{"pid",getpid()}});
    else if (target == "/api/state") Reply(fd,200,App::State());
    else if (target == "/api/processes") Reply(fd,200,App::Processes());
    else if (target == "/api/files") Reply(fd,200,App::Files());
    else if (target.starts_with("/api/file?name=")) {
        const auto name = Decode(target.substr(15));
        const auto path = App::ResolveFile(name);
        if (path.empty()) Reply(fd,404,{{"error","File not found"}});
        else File(fd,path,"application/octet-stream",true);
    } else {
        static const std::map<std::string,std::string> assets{{"/","index.html"},{"/index.html","index.html"},{"/app.js","app.js"},{"/style.css","style.css"},{"/styles.css","styles.css"}};
        const auto found = assets.find(target.substr(0,target.find('?')));
        if (found == assets.end()) { Reply(fd,404,{{"error","Not found"}}); return; }
        const auto& name = found->second;
        File(fd, webRoot + "/" + name, name.ends_with(".js") ? "text/javascript; charset=utf-8" : name.ends_with(".css") ? "text/css; charset=utf-8" : "text/html; charset=utf-8", false);
    }
}
}
int RunHttpServer(unsigned short port,const std::string& webRoot,std::atomic_bool& stop) {
    // sendfile has no MSG_NOSIGNAL flag; a cancelled browser download must not
    // terminate the service (or the active dump worker).
    struct sigaction ignore{}; ignore.sa_handler=SIG_IGN; sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE,&ignore,nullptr);
    Socket listener{socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0)};
    if (listener.fd < 0) return 1;
    int reuse=1; setsockopt(listener.fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(port); addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (bind(listener.fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)) || listen(listener.fd,16)) { perror("HTTP bind/listen"); return 1; }
    fcntl(listener.fd,F_SETFL,fcntl(listener.fd,F_GETFL)|O_NONBLOCK);
    printf("UMT HTTP http://127.0.0.1:%hu\n",port);
    std::vector<std::thread> pool;
    for (int i=0;i<4;++i) pool.emplace_back([&] {
        while (!stop) {
            pollfd p{listener.fd,POLLIN,0};
            if (poll(&p,1,250)<=0) continue;
            Socket client{accept4(listener.fd,nullptr,nullptr,SOCK_CLOEXEC)};
            if (client.fd<0) continue;
            try { Handle(client.fd,webRoot); }
            catch (const std::exception& error) { Reply(client.fd,400,{{"error",error.what()}}); }
        }
    });
    for (auto& thread:pool) thread.join();
    return 0;
}
