#pragma once
#include "Arduino.h"
#include <functional>
#include <map>
#include <string>
constexpr int HTTP_POST = 1;
class WebServer {
 public:
  std::map<std::string, String> args, headers;
  std::map<std::string, std::function<void()>> routes;
  int status = 0;
  String body;
  void on(const char* path, int, std::function<void()> handler) { routes[path] = handler; }
  bool hasArg(const char* name) { return args.count(name); }
  String arg(const char* name) { return args[name]; }
  bool hasHeader(const char* name) { return headers.count(name); }
  String header(const char* name) { return headers[name]; }
  void send(int code, const char*, const String& text) {
    status = code;
    body = text;
  }
  void request(const char* path) {
    status = 0;
    body.clear();
    routes.at(path)();
  }
};
