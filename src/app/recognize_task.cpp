#include "recognize_task.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <string>

#include <QThread>

#include <quface/logger.hpp>

#include "config.hpp"
#include "record_task.hpp"

using namespace suanzi;

RecognizeTask *RecognizeTask::get_instance() {
  static RecognizeTask instance;
  return &instance;
}

bool RecognizeTask::idle() { return !get_instance()->is_running_; }

RecognizeTask::RecognizeTask(QThread *thread, QObject *parent)
    : is_running_(false) {
  auto cfg = Config::get_quface();
  face_database_ = std::make_shared<FaceDatabase>(cfg.db_name);

  face_extractor_ = std::make_shared<FaceExtractor>(cfg.model_file_path);
  anti_spoofing_ = std::make_shared<FaceAntiSpoofing>(cfg.model_file_path);
  mask_detector_ = std::make_shared<MaskDetector>(cfg.model_file_path);

  // Initialize PINGPANG buffer
  Size size_bgr_1 = VPSS_CH_SIZES_BGR[1];
  Size size_bgr_2 = VPSS_CH_SIZES_BGR[2];
  if (CH_ROTATES_BGR[1]) {
    size_bgr_1.height = VPSS_CH_SIZES_BGR[1].width;
    size_bgr_1.width = VPSS_CH_SIZES_BGR[1].height;
  }
  if (CH_ROTATES_BGR[2]) {
    size_bgr_2.height = VPSS_CH_SIZES_BGR[2].width;
    size_bgr_2.width = VPSS_CH_SIZES_BGR[2].height;
  }

  Size size_nir_1 = VPSS_CH_SIZES_NIR[1];
  Size size_nir_2 = VPSS_CH_SIZES_NIR[2];
  if (CH_ROTATES_NIR[1]) {
    size_nir_1.height = VPSS_CH_SIZES_NIR[1].width;
    size_nir_1.width = VPSS_CH_SIZES_NIR[1].height;
  }
  if (CH_ROTATES_NIR[2]) {
    size_nir_2.height = VPSS_CH_SIZES_NIR[2].width;
    size_nir_2.width = VPSS_CH_SIZES_NIR[2].height;
  }

  buffer_ping_ =
      new RecognizeData(size_bgr_1, size_bgr_2, size_nir_1, size_nir_2);
  buffer_pang_ =
      new RecognizeData(size_bgr_1, size_bgr_2, size_nir_1, size_nir_2);
  pingpang_buffer_ =
      new PingPangBuffer<RecognizeData>(buffer_ping_, buffer_pang_);

  // Create thread
  if (thread == nullptr) {
    static QThread new_thread;
    moveToThread(&new_thread);
    new_thread.start();
  } else {
    moveToThread(thread);
    thread->start();
  }

  rx_nir_finished_ = false;
  rx_bgr_finished_ = false;
}

RecognizeTask::~RecognizeTask() {
  if (buffer_ping_) delete buffer_ping_;
  if (buffer_pang_) delete buffer_pang_;
  if (pingpang_buffer_) delete pingpang_buffer_;
}

void RecognizeTask::rx_frame(PingPangBuffer<DetectionData> *buffer) {
  is_running_ = true;

  // copy from input to output
  buffer->switch_buffer();
  DetectionData *input = buffer->get_pang();
  RecognizeData *output = pingpang_buffer_->get_ping();
  input->copy_to(*output);

  output->bgr_face_detected_ = input->bgr_face_detected_;
  output->nir_face_detected_ = input->nir_face_detected_;
  output->bgr_detection_ = input->bgr_detection_;
  output->nir_detection_ = input->nir_detection_;
  output->has_live = !rx_nir_finished_;
  output->has_person_info = !rx_bgr_finished_;

  if (input->bgr_face_valid()) {
    if (output->has_live) {
      if (!Config::enable_anti_spoofing())
        output->is_live = true;
      else
        output->is_live = is_live(input);
    }
    if (output->has_person_info) {
      output->has_mask = has_mask(input);
      extract_and_query(input, output->has_mask, output->person_feature,
                        output->person_info);
    }
  } else {
    output->has_live = false;
    output->has_person_info = false;
  }

  if (RecordTask::idle())
    emit tx_frame(pingpang_buffer_);
  else
    QThread::usleep(10);

  is_running_ = false;
}

void RecognizeTask::rx_nir_finish(bool if_finished) {
  rx_nir_finished_ = if_finished;
}

void RecognizeTask::rx_bgr_finish(bool if_finished) {
  rx_bgr_finished_ = if_finished;
}

bool RecognizeTask::is_live(DetectionData *detection) {
  if (detection->nir_face_valid()) {
    int width = detection->img_bgr_large->width;
    int height = detection->img_bgr_large->height;

    suanzi::FaceDetection face_detection;
    suanzi::FacePose pose;
    detection->bgr_detection_.scale(width, height, face_detection, pose);

    SZ_BOOL is_live;
    SZ_RETCODE ret = anti_spoofing_->validate(
        (const SVP_IMAGE_S *)detection->img_bgr_large->pImplData,
        face_detection, is_live, Config::get_user().antispoof_score);

    if (SZ_RETCODE_OK == ret && is_live == SZ_TRUE)
      return true;
    else
      return false;
  } else
    return false;
}

bool RecognizeTask::has_mask(DetectionData *detection) {
  int width = detection->img_bgr_large->width;
  int height = detection->img_bgr_large->height;

  suanzi::FaceDetection face_detection;
  suanzi::FacePose pose;
  detection->bgr_detection_.scale(width, height, face_detection, pose);

  SZ_BOOL has_mask;
  SZ_RETCODE ret = mask_detector_->classify(
      (const SVP_IMAGE_S *)detection->img_bgr_large->pImplData, face_detection,
      has_mask, Config::get_user().mask_score);

  if (SZ_RETCODE_OK == ret && has_mask == SZ_TRUE)
    return true;
  else
    return false;
}

void RecognizeTask::extract_and_query(DetectionData *detection, bool has_mask,
                                      FaceFeature &feature,
                                      QueryResult &person_info) {
  int width = detection->img_bgr_large->width;
  int height = detection->img_bgr_large->height;

  suanzi::FaceDetection face_detection;
  suanzi::FacePose pose;
  detection->bgr_detection_.scale(width, height, face_detection, pose);

  // extract: 25ms
  SZ_RETCODE ret = face_extractor_->extract(
      (const SVP_IMAGE_S *)detection->img_bgr_large->pImplData, face_detection,
      pose, feature);

  if (SZ_RETCODE_OK == ret) {
    // query
    static std::vector<suanzi::QueryResult> results;
    results.clear();

    ret = face_database_->query(feature, 1, results);
    if (SZ_RETCODE_OK == ret) {
      if (has_mask)
        person_info.score = pow((results[0].score - 0.5) * 2, 0.45) / 2 + 0.5;
      else
        person_info.score = results[0].score;
      person_info.face_id = results[0].face_id;
      // SZ_LOG_INFO("mask={}, id={}, score={:.2f}", has_mask,
      // person_info.face_id,
      //             person_info.score);
      return;
    }
  }

  // SZ_RETCODE_EMPTY_DATABASE or SZ_RETCODE_FAILED
  person_info.score = 0;
  person_info.face_id = 0;
}
