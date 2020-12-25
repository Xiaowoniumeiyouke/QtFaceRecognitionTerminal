#include "detect_tip_widget.hpp"

#include <QPaintEvent>
#include <QPainter>
#include <QPicture>
#include <QTimer>

#include "config.hpp"

using namespace suanzi;

DetectTipWidget::DetectTipWidget(int win_x, int win_y, int win_width,
                                 int win_height, QWidget *parent)
    : win_x_(win_x),
      win_y_(win_y),
      win_width_(win_width),
      win_height_(win_height),
      lost_age_(0),
      is_valid_(false),
      QWidget(parent) {
  detect_pos_x_ = 0.0;
  detect_pos_y_ = 0.0;
  detect_width_ = 0.0;
  detect_height_ = 0.0;

  QPalette palette = this->palette();
  palette.setColor(QPalette::Background, Qt::transparent);
  setPalette(palette);

  // move(win_x_, win_y_);
  // setFixedSize(win_width_, win_height_);
  setGeometry(detect_pos_x_ + win_x_, detect_pos_y_ + win_y_, detect_width_,
              detect_height_);
}

DetectTipWidget::~DetectTipWidget() {}

void DetectTipWidget::paintEvent(QPaintEvent *event) {
  QPainter painter(this);
  paint(&painter);
}

void DetectTipWidget::get_detect_position(float &x, float &y, float &width,
                                          float &height) {
  if (rects_.size() > 0) {
    float sum_x = 0, sum_y = 0, sum_width = 0, sum_height = 0;
    float count = 0;
    auto it = rects_.end();
    QRect latest = rects_.back();
    do {
      it -= 1;

      QRect inter_section = latest.intersected(*it);
      QRect united_section = latest.united(*it);

      float iou = 1.f * inter_section.width() * inter_section.height() /
                  united_section.width() / united_section.height();

      float weight = pow(iou, 6);

      sum_x += weight * it->x();
      sum_y += weight * it->y();
      sum_width += weight * it->width();
      sum_height += weight * it->height();
      count += weight;

    } while (it != rects_.begin());

    width = sum_width / count;
    height = sum_height / count;
    x = sum_x / count;
    y = sum_y / count;
  }
}

void DetectTipWidget::paint(QPainter *painter) {
  // if (rects_.size() > 0 && !Config::get_user().enable_temperature) {
  if (rects_.size() > 0) {
    float width = detect_width_;
    float height = detect_height_;
    float top_x = 1.0;
    float top_y = 1.0;
    float bottom_x = top_x + width - 2.0;
    float bottom_y = top_y + height - 2.0;

    if (is_valid_)
      painter->setPen(QPen(QColor(0, 0, 255, 128), 5));
    else
      painter->setPen(QPen(QColor(255, 0, 0, 128), 5));
    painter->drawLine(top_x, top_y, top_x + width / 5, top_y);
    painter->drawLine(top_x, top_y, top_x, top_y + height / 5);
    painter->drawLine(top_x, bottom_y, top_x + width / 5, bottom_y);
    painter->drawLine(top_x, bottom_y, top_x, bottom_y - height / 5);
    painter->drawLine(bottom_x, top_y, bottom_x, top_y + height / 5);
    painter->drawLine(bottom_x, top_y, bottom_x - width / 5, top_y);
    painter->drawLine(bottom_x, bottom_y, bottom_x, bottom_y - height / 5);
    painter->drawLine(bottom_x, bottom_y, bottom_x - width / 5, bottom_y);

#ifdef DEBUG
    if (pose_.length() > 0) {
      painter->setPen(Qt::red);
      painter->drawText(top_x, top_y - 20, pose_);
      for (size_t i = 0; i < SZ_LANDMARK_NUM; i++)
        painter->drawEllipse(landmarks_[i], 2, 2);
    }
#endif
  }
}

void DetectTipWidget::rx_display(DetectionRatio detection, bool to_clear,
                                 bool valid, bool is_bgr) {
  /*if (Config::get_user().enable_temperature) {
    hide();
    return;
  }*/

  int box_x = 1;
  int box_y = 1;
  int box_w = win_width_ - 1;
  int box_h = win_height_ - 1;

  // fix screen scale
  float ratio = 1.f * win_height_ / win_width_ * 1080 / 1920;
  float cy = detection.y + detection.height / 2;

  if (Config::get_camera(CAMERA_BGR).rotate == 1) {
    detection.y = detection.y / ratio;
    detection.height = detection.height / ratio;
  } else {
    detection.y = detection.y * (ratio + cy * (1 - ratio));
    detection.height = detection.height * (ratio + cy * (1 - ratio));
  }

  if (!to_clear) {
    // display pose for debug
    landmarks_.clear();
    if (is_bgr) {
      for (int i = 0; i < SZ_LANDMARK_NUM; i++)
        landmarks_.push_back({(int)(detection.landmark[i][0] * box_w),
                              (int)(detection.landmark[i][1] * box_h)});

      pose_ = "Yaw: " + QString::number(detection.yaw, 'f', 1) +
              " Pitch: " + QString::number(detection.pitch, 'f', 1) +
              " Roll: " + QString::number(detection.roll, 'f', 1);
    } else
      pose_ = "";

    // resize to square
    float center_x = box_x + (detection.x + .5f * detection.width) * box_w;
    float center_Y = box_y + (detection.y + .5f * detection.height) * box_h;
    float size = .5f * (detection.width * box_w + detection.height * box_h);

    QRect next_rect(center_x - .5f * size, center_Y - .5f * size, size, size);
    rects_.push_back(next_rect);
    if (rects_.size() > MAX_RECT_COUNT) rects_.erase(rects_.begin());

    if (is_bgr) is_valid_ = valid;

    get_detect_position(detect_pos_x_, detect_pos_y_, detect_width_,
                        detect_height_);
    setGeometry(detect_pos_x_ + win_x_, detect_pos_y_ + win_y_, detect_width_,
                detect_height_);
    show();
  } else {
    if (++lost_age_ > MAX_LOST_AGE) {
      rects_.clear();
      lost_age_ = 0;
      is_valid_ = false;
    }
    hide();
  }
}
