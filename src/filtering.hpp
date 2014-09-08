
#pragma once

//
// OpenCVによるノイズ除去
//

#include "defines.hpp"
#include <string>
#include <opencv2/opencv.hpp>
#include "json.hpp"


// リンクするライブラリの定義(Windows)
#if defined (_MSC_VER)
#ifdef _DEBUG
#pragma comment (lib, "opencv_core249d.lib")
#pragma comment (lib, "opencv_highgui249d.lib")
#pragma comment (lib, "opencv_imgproc249d.lib")
#pragma comment (lib, "opencv_photo249d.lib")
#pragma comment (lib, "libpngd.lib")
#pragma comment (lib, "zlibd.lib")
#else
#pragma comment (lib, "opencv_core249.lib")
#pragma comment (lib, "opencv_highgui249.lib")
#pragma comment (lib, "opencv_imgproc249.lib")
#pragma comment (lib, "opencv_photo249.lib")
#pragma comment (lib, "libpng.lib")
#pragma comment (lib, "zlib.lib")
#endif
#endif


namespace {

void doFiltering(const std::string& in_file, const std::string& out_file,
                 const picojson::value& params) {
  cv::Mat src = cv::imread(in_file, cv::IMREAD_COLOR);

  Real noise_sigma = params.at("noise_sigma").get<double>();

  int template_window_size = params.at("template_window_size").get<double>();
  int search_window_size   = params.at("search_window_size").get<double>();

  cv::Mat dst;
  cv::fastNlMeansDenoisingColored(src, dst,
                                  noise_sigma, noise_sigma,
                                  template_window_size, search_window_size);

  cv::imwrite(out_file, dst);
}

}
