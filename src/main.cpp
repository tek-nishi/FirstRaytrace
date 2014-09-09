//
// はじめてのレイトレース
//

#include "defines.hpp"
#include <cassert>
#include <future>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <deque>
#include "appEnv.hpp"
#include "json.hpp"
#include "sceneLoader.hpp"
#include "preview.hpp"
#include "pathtrace.hpp"
#include "os.hpp"
#include "filtering.hpp"
#include "bvh.hpp"


int main() {
  // OS依存実装
  Os os;
  
  // 設定をJsonから読み込む
  auto params = Json::read(os.documentPath() + "res/params.json");


#ifdef DEBUG
  if (params.at("filter_test").get<bool>()) {
    std::string save_path{ os.documentPath() + "progress" };

    // OpenCVによるノイズ除去
    doFiltering(save_path + "/incompletion.png",
                save_path + "/completion.png",
                params);

    return 0;
  }
#endif

  
  const int window_width  = params.at("window_width").get<double>();
  const int window_height = params.at("window_height").get<double>();
  
  const int tile_width  = params.at("tile_width").get<double>();
  const int tile_height = params.at("tile_height").get<double>();

  // FIXME:MacBook Air では 8 が一番速かった
  const int thread_num = params.at("thread_num").get<double>();
  
  AppEnv app_env{ window_width, window_height };

  auto scene = SceneLoader::load(os.documentPath() + "res/" + params.at("path").get<std::string>());

  
#ifdef DEBUG
  // Cheetah3Dが書き出すColladaはIORを含んでいないので、検証用に設定
  if (params.contains("ior_value")) {
    auto& model = scene.model;
    auto& materials = model.material();
    Real ior_value = params.at("ior_value").get<double>();
    for (auto& m : materials) {
      m.ior(ior_value);
    }
  }
#endif

  
  std::string save_path{ os.documentPath() + "progress" };
  Os::createDirecrory(save_path);

  // レンダリング結果の格納先
  std::vector<u_char> row_image(window_height * window_width * 3);
  std::fill(row_image.begin(), row_image.end(), 255);

  // posToWorldで使うviewportの準備
  std::vector<GLint> viewport{ 0, 0, window_width, window_height };

  // 最終画像をタイル状に区切ってレンダリング
  // TIPS:レンダリング途中でスレッドを生成するので
  //      あらかじめ座標データだけを生成
  std::vector<std::future<bool> > future;
  std::deque<std::pair<Vec2i, Vec2i> > tile_info;

  int   height       = window_height;
  Vec2i render_start = Vec2i::Zero();

  while (height > 0) {
    int width        = window_width;
    render_start.x() = 0;

    while (width > 0) {
      Vec2i render_size{ std::min(width, tile_width),
                         std::min(height, tile_height) };
      tile_info.push_back(std::make_pair(render_start, render_size));
      
      render_start.x() += tile_width;
      width            -= tile_width;
    }
    
    render_start.y() += tile_height;
    height           -= tile_height;
  }

  // BVH構築
  auto bvh_node = Bvh::createFromModel(scene.model);

  // Halton列で使うシャッフル列の生成
  std::vector<std::vector<int> > perm_table = faurePermutation(100);
  

  // 複数スレッドでのレンダリング準備
  Pathtrace::RenderInfo info = {
    { window_width, window_height },

    { 0, 0 },
    { window_width, window_height },

    viewport,
    scene.camera,
    scene.ambient,
    scene.lights,
    scene.model,
    bvh_node,
    
    perm_table,

    int(params.at("subpixel_num").get<double>()),
    int(params.at("sample_num").get<double>()),
    int(params.at("recursive_depth").get<double>()),

    params.at("focal_distance").get<double>(),
    params.at("lens_radius").get<double>(),
    
    params.at("exposure").get<double>(),
  };

  // カメラの内部行列を生成
  //   posToWorldで使う
  info.camera(Vec2f{ window_width, window_height });

  std::mutex mutex;
  
  const std::chrono::seconds wait_time(0);
  const std::chrono::seconds write_time(int(params.at("write_time").get<double>()));
  const std::chrono::seconds sleep_time(int(params.at("sleep_time").get<double>()));

  // 一定時間ごとにPNG書き出しをおこなうための準備
  auto render_begin = std::chrono::steady_clock::now();
  auto begin = render_begin;

  // OpenGLでのプレビュー準備
  Preview::setup(scene.lights, scene.ambient);
  // プレビュー時にカメラを変更するのでコピーしておく
  Camera3D preview_camera = scene.camera;

  // 一定時間ごとに途中経過をPNGに書き出す
  int png_index = 1;
  while (1) {
    if (!app_env.isOpen()) break;

    // スレッドの完了チェック
    // TIPS:初回だけ、"コンテナが空"も登録条件にする
    bool render_complete = future.empty();
    for (auto& f : future) {
      auto result = f.wait_for(wait_time);

      if (result == std::future_status::ready) {
        // 結果を空読み
        f.get();

        render_complete = true;
      }
    }

    if (render_complete) {
      // 処理が終わったものはコンテナから除外
      future.erase(std::remove_if(future.begin(),
                                  future.end(),
                                  [](const std::future<bool>& f) { return !f.valid(); }),
                   future.end());

      if (!tile_info.empty()) {
        // 残り領域をレンダリングスレッドへ登録
        int num = std::min(thread_num - future.size(),
                           tile_info.size());

        while (num > 0) {
          const auto& ti = tile_info.cbegin();
        
          info.render_start = ti->first;
          info.render_size  = ti->second;
  
          // レンダリング用のスレッドを準備
          std::packaged_task<bool()> task(std::bind(Pathtrace::render,
                                                    std::ref(mutex),
                                                    std::ref(row_image),
                                                    info));

          DOUT << "render start:"
               << info.render_start.x() << "," << info.render_start.y()
               << " "
               << info.render_size.x() << "," << info.render_size.y()
               << std::endl;
        
          // レンダリング用スレッドの生成
          // std::futureで結果をみているのでdetachして構わない
          future.push_back(task.get_future());
          std::thread render_thread{ std::move(task) };
          render_thread.detach();

          tile_info.pop_front();
          --num;
        }
      }
    }
      
    if (future.empty()) {
      // レンダリングスレッドの動作が完了したら結果を出力
      std::lock_guard<std::mutex> lock(mutex);

      WritePng(save_path + "/incompletion.png",
               window_width, window_height,
               &row_image[0]);

      // 所要時間を計算
      auto current = std::chrono::steady_clock::now();
      auto end = std::chrono::duration_cast<std::chrono::milliseconds>(current - render_begin);

      DOUT << "Render time (sec):" << end.count() / 1000.0f << std::endl;

      // OpenCVによるノイズ除去
      if (params.at("noise_filter").get<bool>()) {
        doFiltering(save_path + "/incompletion.png",
                    save_path + "/completion.png",
                    params);
        
      }
      break;
    }

    auto current      = std::chrono::steady_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::seconds>(current - begin);
    if (elapsed_time > write_time) {
      // 途中経過を出力
      std::lock_guard<std::mutex> lock(mutex);

      std::ostringstream path;
      path << save_path << "/" << std::setw(2) << std::setfill('0') << png_index << ".png";

      WritePng(path.str(),
               window_width, window_height,
               &row_image[0]);

      png_index += 1;
      begin = current;
    }

    // 適当にスリープしてからOpenGLプレビュー
    std::this_thread::sleep_for(sleep_time);
    Preview::display(app_env,
                     preview_camera, scene.lights, scene.model);
  }
  
  // TIPS:detachしたスレッド含めて即時終了
  // FIXME:Xcodeでサポートされていない
  // std::quick_exit(0);
}
