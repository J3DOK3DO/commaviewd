#include "http_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace commaview::api {
namespace {

constexpr int kReadTimeoutSec = 5;
constexpr size_t kMaxRequestBytes = 1024 * 1024;

bool set_close_on_exec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

std::string trim_copy(const std::string& in) {
  size_t s = 0;
  while (s < in.size() && std::isspace(static_cast<unsigned char>(in[s]))) s++;
  size_t e = in.size();
  while (e > s && std::isspace(static_cast<unsigned char>(in[e - 1]))) e--;
  return in.substr(s, e - s);
}

std::string lowercase_copy(std::string in) {
  for (char& c : in) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return in;
}

std::string reason_phrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "OK";
  }
}

bool recv_into_buffer(int fd, std::string* buf) {
  char tmp[4096];
  ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
  if (n <= 0) return false;
  buf->append(tmp, static_cast<size_t>(n));
  return true;
}

bool parse_request(const std::string& raw, HttpRequest* req, std::string* error) {
  const size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    *error = "incomplete headers";
    return false;
  }

  const std::string header_block = raw.substr(0, header_end);
  std::istringstream stream(header_block);
  std::string line;

  if (!std::getline(stream, line)) {
    *error = "missing request line";
    return false;
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();

  std::istringstream req_line(line);
  std::string version;
  if (!(req_line >> req->method >> req->path >> version)) {
    *error = "bad request line";
    return false;
  }

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;

    std::string key = lowercase_copy(trim_copy(line.substr(0, colon)));
    std::string value = trim_copy(line.substr(colon + 1));
    req->headers[key] = value;
  }

  size_t content_len = 0;
  auto it = req->headers.find("content-length");
  if (it != req->headers.end()) {
    content_len = static_cast<size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
  }

  const size_t body_start = header_end + 4;
  if (raw.size() < body_start + content_len) {
    *error = "incomplete body";
    return false;
  }

  req->body = raw.substr(body_start, content_len);
  return true;
}

// Handler responses and parse failures share this header block. The bare 413 for an
// oversized request is written separately, without CORS or Content-Type headers.
std::string serialize_response(const HttpResponse& resp) {
  std::ostringstream out;
  out << "HTTP/1.1 " << resp.status << " " << reason_phrase(resp.status) << "\r\n";
  out << "Content-Type: " << resp.content_type << "\r\n";
  out << "Access-Control-Allow-Origin: *\r\n";
  out << "Access-Control-Allow-Headers: Content-Type, X-CommaView-Token\r\n";
  for (const auto& kv : resp.headers) {
    out << kv.first << ": " << kv.second << "\r\n";
  }
  out << "Content-Length: " << resp.body.size() << "\r\n\r\n";
  out << resp.body;
  return out.str();
}

bool send_all(int fd, const char* data, size_t len) {
  while (len > 0) {
    ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
    if (n < 0) return false;
    data += static_cast<size_t>(n);
    len -= static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

HttpServer::HttpServer(int port, RequestHandler handler)
    : port_(port), handler_(std::move(handler)) {}

HttpServer::~HttpServer() {
  stop();
}

bool HttpServer::start(std::string* error) {
  // Close-on-exec from the start: a helper forked by another thread meanwhile must not inherit :5002.
  server_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (server_fd_ < 0) {
    if (error) *error = "socket failed";
    return false;
  }
  if (!set_close_on_exec(server_fd_)) {
    if (error) *error = "fcntl(FD_CLOEXEC) failed";
    ::close(server_fd_);
    server_fd_ = -1;
    return false;
  }

  int opt = 1;
  setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(port_));

  if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    if (error) {
      *error = "bind failed";
      *error += std::string(": ") + std::strerror(errno);
    }
    ::close(server_fd_);
    server_fd_ = -1;
    return false;
  }

  if (::listen(server_fd_, 16) < 0) {
    if (error) *error = "listen failed";
    ::close(server_fd_);
    server_fd_ = -1;
    return false;
  }

  running_ = true;
  return true;
}

void HttpServer::serve_forever() {
  while (running_) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    // Close-on-exec: a helper started while answering (an install, the deferred queue's waiter)
    // must not keep the app's connection open.
    int client_fd = ::accept4(server_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (running_) std::perror("accept");
      continue;
    }

    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    timeval tv{};
    tv.tv_sec = kReadTimeoutSec;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    handle_client(client_fd);
    ::close(client_fd);
  }
}

void HttpServer::stop() {
  if (!running_) return;
  running_ = false;
  if (server_fd_ >= 0) {
    ::shutdown(server_fd_, SHUT_RDWR);
    ::close(server_fd_);
    server_fd_ = -1;
  }
}

bool HttpServer::handle_client(int client_fd) {
  std::string raw;
  while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < kMaxRequestBytes) {
    if (!recv_into_buffer(client_fd, &raw)) return false;
  }

  if (raw.size() >= kMaxRequestBytes) {
    const std::string resp = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\n\r\n";
    return send_all(client_fd, resp.data(), resp.size());
  }

  size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) return false;

  size_t content_len = 0;
  {
    std::string headers = raw.substr(0, header_end);
    std::istringstream hs(headers);
    std::string line;
    std::getline(hs, line);  // request line
    while (std::getline(hs, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      size_t c = line.find(':');
      if (c == std::string::npos) continue;
      std::string key = lowercase_copy(trim_copy(line.substr(0, c)));
      if (key == "content-length") {
        content_len = static_cast<size_t>(std::strtoul(trim_copy(line.substr(c + 1)).c_str(), nullptr, 10));
        break;
      }
    }
  }

  while (raw.size() < header_end + 4 + content_len && raw.size() < kMaxRequestBytes) {
    if (!recv_into_buffer(client_fd, &raw)) break;
  }

  HttpRequest req;
  std::string parse_error;
  if (!parse_request(raw, &req, &parse_error)) {
    HttpResponse bad;
    bad.status = 400;
    bad.body = "{\"ok\":false,\"error\":\"bad request\"}";
    const std::string wire = serialize_response(bad);
    return send_all(client_fd, wire.data(), wire.size());
  }

  const std::string wire = serialize_response(handler_(req));
  return send_all(client_fd, wire.data(), wire.size());
}

}  // namespace commaview::api
