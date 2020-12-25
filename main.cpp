#include <QFile>
#include <QTranslator>
#include <QtWidgets/QApplication>

#include "face_server.hpp"
#include "http_server.hpp"
#include "led_task.hpp"
#include "video_player.hpp"

using namespace suanzi;

void load_translator(QApplication& app) {
  static std::string last_lang = Config::get_user_lang();

  std::string lang = Config::get_user_lang();

  if (last_lang != lang) {
    SZ_LOG_INFO("Load translator for lang={}", lang);

    static QTranslator translator;
    QString new_lang_file = (":face_terminal_" + lang).c_str();
    if (!translator.load(new_lang_file)) {
      SZ_LOG_WARN("translator load failed for lang={}", lang);
    }
    app.installTranslator(&translator);

    AudioTask::get_instance()->load_audio();
  }

  last_lang = lang;
}

void trigger_led() {
  bool last_status = LEDTask::get_status();
  bool status = Config::get_user().enable_led;
  if (status && !last_status) LEDTask::get_instance()->turn_on(2000);
  if (!status) LEDTask::get_instance()->turn_off(true);
}

void trigger_gpio() {
  if (Config::get_user().relay_default_state == RelayState::Low)
    Engine::instance()->gpio_set(GpioPinDOOR, false);
  else
    Engine::instance()->gpio_set(GpioPinDOOR, true);
}

void reset_temperature() {
  static float last_bias = Config::get_temperature_bias();
  float bias = Config::get_temperature_bias();
  if (std::abs(bias - last_bias > 0.1)) RecordTask::clear_temperature();

  last_bias = bias;
}

Config* read_cfg(int argc, char* argv[]) {
  // 基础配置文件，默认：config.json
  std::string cfg_file = "config.json";

  // 动态加载配置文件，默认：config.override.json
  std::string cfg_override_file = "config.override.json";

  // 读取命令行参数，格式为
  // ./face-terminal --config config.json --override-config config.override.json
  for (int i = 1; i < argc; i++) {
    auto arg = std::string(argv[i]);
    if (i < argc - 1) {
      if (arg == "-c" || arg == "--config") {
        i++;
        cfg_file = argv[i];
      } else if (arg == "-cc" || arg == "--override-config") {
        i++;
        cfg_override_file = argv[i];
      }
    }
  }

  // 加载配置文件
  auto config = Config::get_instance();
  if (SZ_RETCODE_OK == config->load_from_file(cfg_file, cfg_override_file))
    return config;
  else
    return NULL;
}

Engine* create_engine() {
  ROTATION_E vo_rotate;
  Config::load_vo_rotation(vo_rotate);

  // 读取屏幕类型
  LCDScreenType lcd_screen_type;
  if (!Config::load_screen_type(lcd_screen_type)) return NULL;

  SensorType sensor0_type = SONY_IMX327_2L_MIPI_2M_30FPS_12BIT;
  SensorType sensor1_type = SONY_IMX327_2L_MIPI_2M_30FPS_12BIT;
  if (!Config::load_sensor_type(sensor0_type, sensor1_type)) return NULL;

  // 读取摄像头参数
  auto bgr_cam = Config::get_camera(CAMERA_BGR);
  auto nir_cam = Config::get_camera(CAMERA_NIR);

  // 读取应用参数
  auto app_cfg = Config::get_app();
  auto user_cfg = Config::get_user();

  EngineOption opt = {
      .bgr =
          {
              .sensor_type = sensor0_type,
              .dev = bgr_cam.index,
              .flip = true,
              .wdr = user_cfg.wdr,
              .channels =
                  {
                      {
                          .index = 0,
                          .rotate = bgr_cam.rotate,
                          .size =
                              {
                                  .width = 1920,
                                  .height = 1080,
                              },
                      },
                      {
                          .index = 1,
                          .rotate = bgr_cam.rotate,
                          .size =
                              {
                                  .width = 1080,
                                  .height = 704,
                              },
                      },
                      {
                          .index = 2,
                          .rotate = bgr_cam.rotate,
                          .size =
                              {
                                  .width = 320,
                                  .height = 224,
                              },
                      },
                  },
          },
      .nir =
          {
              .sensor_type = sensor1_type,
              .dev = nir_cam.index,
              .flip = true,
              .wdr = user_cfg.wdr,
              .channels =
                  {
                      {
                          .index = 0,
                          .rotate = nir_cam.rotate,
                          .size =
                              {
                                  .width = 1920,
                                  .height = 1080,
                              },
                      },
                      {
                          .index = 1,
                          .rotate = nir_cam.rotate,
                          .size =
                              {
                                  .width = 1080,
                                  .height = 704,
                              },
                      },
                      {
                          .index = 2,
                          .rotate = nir_cam.rotate,
                          .size =
                              {
                                  .width = 320,
                                  .height = 224,
                              },
                      },
                  },
          },
      .screen =
          {
              .type = lcd_screen_type,
              .rotate = vo_rotate,
          },
      .show_secondary_win = app_cfg.show_infrared_window,
      .secondary_win_percent =
          (SecondaryWinPercent)app_cfg.infrared_window_percent,
  };

  auto engine = Engine::instance();
  engine->set_option(opt);

  return engine;
}

VideoPlayer* create_gui() {
  // 预加载 Quface 算法模块
  auto quface = Config::get_quface();
  std::make_shared<FaceDetector>(quface.model_file_path);
  std::make_shared<FaceExtractor>(quface.model_file_path);
  std::make_shared<FacePoseEstimator>(quface.model_file_path);
  std::make_shared<FaceAntiSpoofing>(quface.model_file_path);
  std::make_shared<MaskDetector>(quface.model_file_path);
  std::make_shared<FaceDatabase>(quface.db_name);

  // 加载 Web 服务模块
  static auto person_service = PersonService::get_instance();
  static auto face_service = std::make_shared<FaceService>(person_service);
  static auto face_server = std::make_shared<FaceServer>(face_service);
  static auto http_server = std::make_shared<HTTPServer>();
  face_server->add_event_source(http_server);

  auto app_cfg = Config::get_app();
  std::thread t(
      [&]() { http_server->run(app_cfg.server_port, app_cfg.server_host); });
  t.detach();

  auto engine = Engine::instance();
  engine->stop_boot_ui();

  return new VideoPlayer();
}

int main(int argc, char* argv[]) {
  // Step 1: 加载配置文件
  auto config = read_cfg(argc, argv);
  if (config == NULL) return -1;

  // Step 2: 必须先创建Engine
  auto engine = create_engine();
  if (engine == NULL) return -1;

  // Step 3: 初始化QT（必须在Engine初始化之后）
  QApplication app(argc, argv);
  //去掉触摸屏光标
  QApplication::setOverrideCursor(Qt::BlankCursor);

  // Step 4: 播放自定义开机画面
  std::vector<SZ_BYTE> img;
  if (Config::read_boot_background(img)) {
    engine->start_boot_ui(img);
  }

  // Step 5: 多语言支持
  load_translator(app);
  config->appendListener("reload", [&app]() { load_translator(app); });

  // Step 4.1: LED配置修改触发反馈
  Engine::instance()->gpio_set(GpioPinLightBox, false);
  trigger_gpio();
  config->appendListener("reload", trigger_led);
  config->appendListener("reload", trigger_gpio);
  config->appendListener("reload", reset_temperature);

  // Step 6: 启动主程序UI
  auto gui = create_gui();
  if (gui == NULL) return -1;
  gui->show();

  std::thread([&]() {
    RTSPOption option = {
        .enable_auth = false,
        .enable_rtsp_over_http = false,
        .http_port = 8000,
        .rtsp_port = 554,
        .max_packet_size = 1456,
        .max_buffer_size = 2 * 1024 * 1024,
        .enable_timestamp = true,
    };

    auto server = engine->start_live_streaming(option);
    if (server) {
      server->run();

      server->stop();
    }
  })
      .detach();

  return app.exec();
}
