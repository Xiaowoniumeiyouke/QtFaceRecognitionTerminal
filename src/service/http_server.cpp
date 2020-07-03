#include "http_server.hpp"

using namespace suanzi;

HTTPServer::HTTPServer() {
  m_svr = std::make_shared<Server>();

  m_svr->set_logger([](const Request& req, const Response& res) {
    SZ_LOG_INFO("HTTP {} {} {} {}", req.method, req.path, req.content_length,
                res.status);
  });

  m_svr->set_error_handler([](const Request& req, Response& res) {
    auto fmt = "<p>Error Status: <span style='color:red;'>%d</span></p>";
    char buf[BUFSIZ];
    snprintf(buf, sizeof(buf), fmt, res.status);
    res.set_content(buf, "text/html");
  });
}

void HTTPServer::run(uint16_t port) {
  SZ_LOG_INFO("Http server license on port {}", port);

  auto handler = [&](const Request& req, Response& res) {
    if (!req.has_header("Content-Type")) {
      json data = {{"ok", false}, {"message", "Content-Type missing"}};
      res.set_content(data.dump(), "application/json");
      return;
    }
    if (req.get_header_value("Content-Type") != "application/json") {
      json data = {{"ok", false},
                   {"message", "content type shoule be application/json"}};
      res.set_content(data.dump(), "application/json");
      return;
    }

    auto method = "db." + req.matches[1].str();
    // SZ_LOG_DEBUG("method={}, body={}", method, req.body);
    auto bodyStr = req.body;
    if (bodyStr.size() == 0) {
      bodyStr = "{}";
    }

    json body;
    try {
      body = json::parse(bodyStr);
    } catch (const std::exception& exc) {
      json data = {{"ok", false, "message", exc.what()}};
      res.set_content(data.dump(), "application/json");
      return;
    }

    try {
      dispatch(method, body, [&](EmitCallbackData data) {
        res.set_content(data.dump(), "application/json");
      });
    } catch (const std::exception& exc) {
      SZ_LOG_ERROR("Message err: {}", exc.what());
    }

    // res.set_content("Hello World!", "application/json");
  };

  m_svr->Post(R"(^/db/(.+))", handler);

  m_svr->listen("0.0.0.0", port);
}