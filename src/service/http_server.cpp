#include "http_server.hpp"

#include <cstdio>
#include <quface-io/engine.hpp>
#include "audio_task.hpp"
#include "gpio_task.hpp"
#include "static_config.hpp"

using namespace suanzi;
using namespace suanzi::io;

HTTPServer::HTTPServer(bool enable_logger) {
  server_ = std::make_shared<Server>();

  if (enable_logger) {
    server_->set_logger([](const Request& req, const Response& res) {
      SZ_LOG_INFO("HTTP {} {} {} {}", req.method, req.path, req.content_length,
                  res.status);
    });
  }

  server_->set_error_handler([](const Request& req, Response& res) {
    auto fmt = "<p>Error Status: <span style='color:red;'>%d</span></p>";
    char buf[BUFSIZ];
    snprintf(buf, sizeof(buf), fmt, res.status);
    res.set_content(buf, "text/html");
  });
}

void HTTPServer::response_failed(Response& res, const std::string& message) {
  json data = {{"ok", false}, {"message", message}};
  res.set_content(data.dump(), "application/json");
}

void HTTPServer::response_ok(Response& res) {
  json data = {{"ok", true}, {"message", "ok"}};
  res.set_content(data.dump(), "application/json");
}

void HTTPServer::run(uint16_t port, const std::string& host) {
  SZ_LOG_INFO("Http server license on port {}", port);

  auto handler = [&](const Request& req, Response& res) {
    if (!req.has_header("Content-Type")) {
      response_failed(res, "Content-Type missing");
      return;
    }
    if (req.get_header_value("Content-Type") != "application/json") {
      response_failed(res, "content type shoule be application/json");
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
      response_failed(res, exc.what());
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

  server_->Post(R"(^/db/(.+))", handler);

  server_->Get("/version", [&](const Request& req, Response& res) {
    json body = {
        {"version", APP_VERSION},
        {"quface_sdk_version", QuFaceSDK_VERSION},
        {"quface_io_sdk_version", QuFaceIOSDK_VERSION},
    };
    res.set_content(body.dump(), "application/json");
  });

  server_->Post("/config", [&](const Request& req, Response& res) {
    try {
      auto cfg = Config::get_instance();
      SZ_RETCODE ret;
      auto j = json::parse(req.body);
      ret = cfg->save_diff(j);
      // cfg->dispatch("led diff");
      if (ret != SZ_RETCODE_OK) {
        response_failed(res, "save error " + std::to_string(ret));
        return;
      }

      auto pre_user = Config::get_user();
      auto pre_app = Config::get_app();

      ret = cfg->reload();
      if (ret != SZ_RETCODE_OK) {
        response_failed(res, "reload error " + std::to_string(ret));
        return;
      }

      auto post_user = Config::get_user();
      auto post_app = Config::get_app();

      if (pre_app.show_infrared_window != post_app.show_infrared_window) {
        if (!Engine::instance()->switch_secondary_window()) {
          SZ_LOG_ERROR("Switch secondary window failed");
        }
      }

      if (pre_user.wdr != post_user.wdr) {
        if (!Engine::instance()->switch_wdr_mode()) {
          SZ_LOG_ERROR("Switch WDR mode failed");
        }
      }

      response_ok(res);
    } catch (const std::exception& exc) {
      SZ_LOG_ERROR("Message err: {}", exc.what());
      response_failed(res, exc.what());
    }
  });

  server_->Get("/config", [&](const Request& req, Response& res) {
    auto cfg = Config::get_all();

    json body(cfg);
    res.set_content(body.dump(), "application/json");
  });

  server_->Get("/config/-/flatten", [&](const Request& req, Response& res) {
    auto cfg = Config::get_all();

    json body(cfg);
    res.set_content(body.flatten().dump(), "application/json");
  });

  server_->Post("/config/-/reset", [&](const Request& req, Response& res) {
    auto cfg = Config::get_instance();

    SZ_RETCODE ret = cfg->reset();
    if (ret != SZ_RETCODE_OK) {
      response_failed(res, "save error " + std::to_string(ret));
      return;
    }

    response_ok(res);
  });

  server_->Post("/background/-/reset", [&](const Request& req, Response& res) {
    auto app = Config::get_app();

    if (std::remove(app.boot_image_path.c_str())) {
      SZ_LOG_ERROR("Remove {} failed", app.boot_image_path);
    } else {
      SZ_LOG_INFO("Remove {} succeed", app.boot_image_path);
    }

    if (std::remove(app.screensaver_image_path.c_str())) {
      SZ_LOG_ERROR("Remove {} failed", app.screensaver_image_path);
    } else {
      SZ_LOG_INFO("Remove {} succeed", app.screensaver_image_path);
    }

    response_ok(res);
  });

  server_->Get("/background", [&](const Request& req, Response& res) {
    std::string type = "boot";
    if (req.has_param("type")) {
      type = req.get_param_value("type");
    }

    std::vector<SZ_BYTE> img;
    if (type == "boot") {
      if (!Config::read_boot_background(img)) {
        response_failed(res, "read failed");
        return;
      }
    } else if (type == "screen-saver") {
      if (!Config::read_screen_saver_background(img)) {
        response_failed(res, "read failed");
        return;
      }
    } else {
      response_failed(res, "unknown type " + type);
      return;
    }

    res.set_content((const char*)img.data(), img.size(), "application/jpeg");
  });

  server_->Post("/background", [&](const Request& req, Response& res) {
    if (!req.is_multipart_form_data()) {
      response_failed(res, "invalid content type");
      return;
    }

    auto type = req.get_file_value("type");
    auto file = req.get_file_value("file");

    auto app = Config::get_app();

    std::string image_name;
    if (type.content == "boot") {
      image_name = app.boot_image_path;
    } else if (type.content == "screen-saver") {
      image_name = app.screensaver_image_path;
    } else {
      response_failed(res, "unknown type " + type.content);
      return;
    }

    std::ofstream fd(image_name, std::ios::binary);
    if (!fd.is_open()) {
      response_failed(res, "open file failed");
      return;
    }

    fd << file.content;

    response_ok(res);
  });

  server_->Get("/audio-volume", [&](const Request& req, Response& res) {
    int volume_percent = 100;
    if (!Config::read_audio_volume(volume_percent)) {
      response_failed(res, "read volume failed");
      return;
    }

    json body = {
        {"ok", true},
        {"percent", volume_percent},
    };
    res.set_content(body.dump(), "application/json");
  });

  server_->Post("/audio-volume", [&](const Request& req, Response& res) {
    try {
      auto cfg = Config::get_instance();
      SZ_RETCODE ret;
      auto j = json::parse(req.body);

      int volume_percent;
      if (!Config::read_audio_volume(volume_percent)) {
        response_failed(res, "read volume failed");
        return;
      }

      if (volume_percent == j["percent"]) return;
      volume_percent = j["percent"];
      if (volume_percent < 0 || volume_percent > 100) {
        response_failed(res, "volume invalid");
        return;
      }

      if (!Config::write_audio_volume(volume_percent)) {
        response_failed(res, "write volume failed");
        return;
      }

      ret = Engine::instance()->audio_set_volume(volume_percent);
      if (ret != SZ_RETCODE_OK) {
        response_failed(res, "set volume failed");
        return;
      }

      auto audio = AudioTask::get_instance();
      if (audio->idle()) audio->beep();

      response_ok(res);
    } catch (const std::exception& exc) {
      SZ_LOG_ERROR("Message err: {}", exc.what());
      response_failed(res, exc.what());
    }
  });

  server_->Post("/audio-play", [&](const Request& req, Response& res) {
    try {
      if (!req.is_multipart_form_data()) {
        response_failed(res, "invalid content type");
        return;
      }

      auto file = req.get_file_value("file");

      auto data =
          std::vector<SZ_UINT8>(file.content.begin(), file.content.end());
      SZ_RETCODE ret;
      ret = Engine::instance()->audio_play(data);
      if (ret != SZ_RETCODE_OK) {
        response_failed(res, "audio play failed");
        return;
      }

      response_ok(res);
    } catch (const std::exception& exc) {
      SZ_LOG_ERROR("Message err: {}", exc.what());
      response_failed(res, exc.what());
    }
  });

  server_->Post("/restart", [&](const Request& req, Response& res) {
    SZ_LOG_WARN("Exit, wait for restart ...");
    raise(SIGTERM);
  });

  server_->Get("/isp/exposure-info", [&](const Request& req, Response& res) {
    auto cam_type = CAMERA_BGR;
    if (req.has_param("cam")) {
      if (req.get_param_value("cam") == "bgr") {
        cam_type = CAMERA_BGR;
      } else {
        cam_type = CAMERA_NIR;
      }
    }

    ISPExposureInfo exp_info;
    SZ_RETCODE ret =
        Engine::instance()->isp_query_exposure_info(cam_type, &exp_info);
    if (ret != SZ_RETCODE_OK) {
      response_failed(res, "get exp info failed");
      return;
    }

    json body(exp_info);
    res.set_content(body.dump(), "application/json");
  });

  server_->Get("/isp/wb-info", [&](const Request& req, Response& res) {
    auto cam_type = CAMERA_BGR;
    if (req.has_param("cam")) {
      if (req.get_param_value("cam") == "bgr") {
        cam_type = CAMERA_BGR;
      } else {
        cam_type = CAMERA_NIR;
      }
    }

    ISPWBInfo wb_info;
    SZ_RETCODE ret = Engine::instance()->isp_query_wb_info(cam_type, &wb_info);
    if (ret != SZ_RETCODE_OK) {
      response_failed(res, "get wb info failed");
      return;
    }

    json body(wb_info);
    res.set_content(body.dump(), "application/json");
  });

  server_->Get("/isp/inner-state-info", [&](const Request& req, Response& res) {
    auto cam_type = CAMERA_BGR;
    if (req.has_param("cam")) {
      if (req.get_param_value("cam") == "bgr") {
        cam_type = CAMERA_BGR;
      } else {
        cam_type = CAMERA_NIR;
      }
    }

    ISPInnerStateInfo inner_state_info;
    SZ_RETCODE ret = Engine::instance()->isp_query_inner_state_info(
        cam_type, &inner_state_info);
    if (ret != SZ_RETCODE_OK) {
      response_failed(res, "get inner state failed");
      return;
    }

    json body(inner_state_info);
    res.set_content(body.dump(), "application/json");
  });

  server_->Post("/trigger-relay", [&](const Request& req, Response& res) {
    auto j = json::parse(req.body);

    if (!j.contains("duration")) {
      response_failed(res, "missing argument duration");
      return;
    }
    int duration = j["duration"];
    if (duration <= 0) {
      response_failed(res, "assert duration > 0 failed");
      return;
    }

    GPIOTask::get_instance()->trigger(duration);
    response_ok(res);
  });

  server_->listen(host.c_str(), port);
}
