#ifndef DEFINE_H
#define DEFINE_H

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "iceoryx_posh/popo/subscriber.hpp"
#include "iceoryx_posh/popo/untyped_subscriber.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"

using namespace iox::runtime;
using namespace iox::popo;

constexpr char CONFIG_PATH[] = "../config/config.ini";
constexpr char APP_NAME[] = "iox-subscriber-Event";

struct CustomCamDataType{
    double timestamp;
    uint32_t width;
    uint32_t height;
    uint8_t data[2048 * 1536 * 3];
};

struct CustomCamDataType2{
    double timestamp;
    uint32_t width;
    uint32_t height;
    cv::Mat data;
};


struct CustomLidarDataType{
    iox::cxx::vector<cv::Point3d, 100000> points;
    iox::cxx::vector<float, 1000> intensitys;
    int size;
    double timestamp;
};

#endif // DEFINE_H
