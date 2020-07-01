#include "detect_task.h"
#include <QRect>
#include <QThread>

#include "quface/common.hpp"
#include "quface/face.hpp"

using namespace suanzi;

DetectTask::DetectTask(QThread *pThread, QObject *parent) {
  face_detector_ = new suanzi::FaceDetector("facemodel.bin");

  if (pThread == nullptr) {
    static QThread thread;
    moveToThread(&thread);
    thread.start();
  } else {
    moveToThread(pThread);
    pThread->start();
  }
}

DetectTask::~DetectTask() {
  if (face_detector_) delete face_detector_;
}

void DetectTask::rxFrame(PingPangBuffer<ImagePackage> *buffer) {
  ImagePackage *pPang = buffer->getPang();
  // printf("DetectTask threadId=%x  %x %d\n", QThread::currentThreadId(), pPang,
        //  pPang->frame_idx);
  // QThread::msleep(100);

  std::vector<suanzi::FaceDetection> detections;
  face_detector_->detect((const SVP_IMAGE_S *)pPang->img_bgr_small->pImplData, detections);
  printf("det size: %d \n", detections.size());
  for (int i = 0; i < detections.size(); i++) {
    auto rect = detections[i].bbox;
    printf("%d: %d %d %d %d \n", i, rect.x, rect.y, rect.width, rect.height);
  }

  buffer->switchToPing();

  // TODO
  // bgr and nir face detection
  // 2 detection results combine
  // send msgs for next steps

  DetectionFloat detection_bgr;
  DetectionFloat detection_nir;
  if(detections.size()>0)
  {
    int w = pPang->img_bgr_small->width;
    int h = pPang->img_bgr_small->height;
    auto rect = detections[0].bbox;
    detection_bgr.x = rect.x*1.0/w;
    detection_bgr.y = rect.y*1.0/h;
    detection_bgr.width = rect.width*1.0/w;
    detection_bgr.height = rect.height*1.0/h;
  }
  emit tx_detection_bgr(buffer, detection_bgr);
  emit tx_detection_nir(buffer, detection_nir);

  // printf("tx1 threadId=%x   %x\n", QThread::currentThreadId(), pPing);
}
