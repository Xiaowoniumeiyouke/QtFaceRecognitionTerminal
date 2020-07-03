#ifndef DETECTTIPWIDGET_H
#define DETECTTIPWIDGET_H

#include <QImage>
#include <QPainter>
#include <QWidget>

#include "detection_float.h"
#include "image_package.h"
#include "pingpang_buffer.h"

namespace suanzi {

class DetectTipWidget : public QWidget {
  Q_OBJECT

 public:
  DetectTipWidget(QWidget *parent = nullptr);
  ~DetectTipWidget() override;

  void paint(QPainter *painter);

 protected:
  void paintEvent(QPaintEvent *event) override;

 private slots:
  void rx_display(DetectionFloat detection);
  void hide_self();

 private:
  static constexpr float MOVING_AVERAGE_RATIO = 1.0;

  QRect rect_;
  float landmark_[5][2];

  bool is_updated_;
};

}  // namespace suanzi

#endif