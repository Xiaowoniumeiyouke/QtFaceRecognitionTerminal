#include <QMetaType>
#include <QtWidgets/QApplication>

#include "face_server.hpp"
#include "http_server.hpp"
#include "video_player.hpp"

using namespace suanzi;

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  auto engine = Engine::instance();

  std::string cfg_file = "config.json";
  std::string cfg_override_file = "config.override.json";
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

  auto config = Config::get_instance();
  SZ_RETCODE ret = config->load_from_file(cfg_file, cfg_override_file);
  if (ret != SZ_RETCODE_OK) {
    return -1;
  }

  auto app_cfg = Config::get_app();
  auto bgr_cam = Config::get_camera(CAMERA_BGR);
  auto nir_cam = Config::get_camera(CAMERA_NIR);
  LCDScreenType lcd_screen_type;
  if (!Config::load_screen_type(lcd_screen_type)) {
    return -1;
  }

  EngineOption opt = {
      .bgr =
          {
              .dev = bgr_cam.index,
              .pipe = bgr_cam.pipe,
              .flip = true,
              .vpss_group = bgr_cam.index,
              .channels =
                  {
                      {
                          .index = 0,
                          .rotate = 0,
                          .size =
                              {
                                  .width = 1920,
                                  .height = 1080,
                              },
                      },
                      {
                          .index = 1,
                          .rotate = 1,
                          .size =
                              {
                                  .width = 1080,
                                  .height = 704,
                              },
                      },
                      {
                          .index = 2,
                          .rotate = 1,
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
              .dev = nir_cam.index,
              .pipe = nir_cam.pipe,
              .flip = true,
              .vpss_group = nir_cam.index,
              .channels =
                  {
                      {
                          .index = 0,
                          .rotate = 0,
                          .size =
                              {
                                  .width = 1920,
                                  .height = 1080,
                              },
                      },
                      {
                          .index = 1,
                          .rotate = 1,
                          .size =
                              {
                                  .width = 1080,
                                  .height = 704,
                              },
                      },
                      {
                          .index = 2,
                          .rotate = 1,
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
          },
      .show_secondary_win = app_cfg.show_infrared_window,
      .secondary_win_percent =
          (SecondaryWinPercent)app_cfg.infrared_window_percent,
  };
  engine->set_option(opt);

  QFile file(":asserts/background.jpg");
  file.open(QIODevice::ReadOnly);

  auto data = file.readAll();
  std::vector<SZ_BYTE> img(data.begin(), data.end());
  engine->start_boot_ui(img);

  auto quface = Config::get_quface();

  auto detector = std::make_shared<FaceDetector>(quface.model_file_path);
  auto extractor = std::make_shared<FaceExtractor>(quface.model_file_path);
  auto pose_estimator =
      std::make_shared<FacePoseEstimator>(quface.model_file_path);
  auto anti_spoof = std::make_shared<FaceAntiSpoofing>(quface.model_file_path);
  auto db = std::make_shared<FaceDatabase>(quface.db_name);

  auto person_service = PersonService::make_shared(
      app_cfg.person_service_base_url, app_cfg.image_store_path);
  auto face_service =
      std::make_shared<FaceService>(db, detector, pose_estimator, extractor,
                                    person_service, app_cfg.image_store_path);
  auto face_server = std::make_shared<FaceServer>(face_service);
  auto http_server = std::make_shared<HTTPServer>();
  face_server->add_event_source(http_server);

  std::thread t(
      [&]() { http_server->run(app_cfg.server_port, app_cfg.server_host); });
  t.detach();

  qRegisterMetaType<PersonData>("PersonData");
  qRegisterMetaType<DetectionRatio>("DetectionRatio");

  engine->stop_boot_ui();

  VideoPlayer player(db, detector, pose_estimator, extractor, anti_spoof,
                     person_service);
  player.show();

  return app.exec();
}
