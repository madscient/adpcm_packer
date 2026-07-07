cmake_minimum_required(VERSION 3.15)

project(adpcm_packer
    VERSION 1.0.0
    DESCRIPTION "WAV to ADPCM binary packer for YM2608/YM2610"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ------------------------------------------------------------
# nlohmann/json  (サブモジュール)
# ------------------------------------------------------------
set(NLOHMANN_JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/nlohmann_json")

if(NOT EXISTS "${NLOHMANN_JSON_DIR}/single_include/nlohmann/json.hpp")
    message(FATAL_ERROR
        "nlohmann/json が見つかりません。\n"
        "サブモジュールを初期化してください:\n"
        "  git submodule update --init --recursive"
    )
endif()

# ヘッダオンリーなのでインターフェースターゲットとして追加
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    "${NLOHMANN_JSON_DIR}/single_include"
)

# ------------------------------------------------------------
# adpcm_packer 実行ファイル
# ------------------------------------------------------------
add_executable(adpcm_packer
    src/main.cpp
    src/codec.cpp
    src/codec.h
    src/wav_reader.h
    src/pitch_estimator.h
)

target_include_directories(adpcm_packer PRIVATE src)
target_link_libraries(adpcm_packer PRIVATE nlohmann_json)

# ------------------------------------------------------------
# コンパイラ別オプション
# ------------------------------------------------------------
if(MSVC)
    target_compile_options(adpcm_packer PRIVATE
        /W4         # 警告レベル4
        /WX         # 警告をエラーとして扱う
        /utf-8      # ソースファイルおよび実行文字セットをUTF-8に設定
        /MP         # マルチプロセッサコンパイル
    )
else()
    # GCC / Clang 共通
    target_compile_options(adpcm_packer PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
    # Release ビルド時の最適化
    target_compile_options(adpcm_packer PRIVATE
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:RelWithDebInfo>:-O2>
    )
endif()

# ------------------------------------------------------------
# インストール
# ------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS adpcm_packer
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
