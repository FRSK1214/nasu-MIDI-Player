cmake_minimum_required(VERSION 3.20)
project(BlackMidiPlayer VERSION 1.0.0 LANGUAGES CXX)

# g++ -std=c++2a -O3 -mwindows -DUNICODE -D_UNICODE -DUSE_SKA_SORT -I. src/main.cpp src/Core/MidiParser.cpp src/Graphics/Renderer.cpp -o BlackMidiPlayer.exe -fopenmp -ld3d11 -ldxgi -ld3dcompiler -ldxguid -lwinmm -lgomp -static -static-libstdc++ -static-libgcc

# C++20設定
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. 依存ライブラリの探索 ---
find_package(OpenMP REQUIRED)

# --- 2. ソースファイルの設定 ---
set(SOURCES
        src/main.cpp
        src/Core/MidiParser.cpp
        src/Graphics/Renderer.cpp
        # ヘッダーファイル（IDE表示用）
        src/Core/NoteDataStore.h
        src/Core/MidiParser.h
        src/Core/Clock.h
        src/Core/MIDIPlayer.h
        src/Graphics/Renderer.h
)

set(SHADERS
        Shaders/NoteVS.hlsl
        Shaders/NotePS.hlsl
)

# --- 3. 実行ファイルの生成 ---
add_executable(BlackMidiPlayer WIN32 ${SOURCES} ${SHADERS})

# --- 4. コンパイルオプションと定義 ---
target_include_directories(BlackMidiPlayer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(BlackMidiPlayer PRIVATE UNICODE _UNICODE USE_SKA_SORT)

# OpenMP用のコンパイルフラグ (-fopenmp) を明示的に追加
if(OpenMP_CXX_FOUND)
    target_compile_options(BlackMidiPlayer PRIVATE ${OpenMP_CXX_FLAGS})
endif()

# --- 5. リンク設定 ---
# MinGWでOpenMPのターゲット指定が漏れるのを防ぐため、直接変数とライブラリ名を指定
target_link_libraries(BlackMidiPlayer PRIVATE
        ${OpenMP_CXX_LIBRARIES}
        d3d11
        dxgi
        d3dcompiler
        dxguid
        winmm
        psapi
)

# 【重要】MinGW環境で依然としてGOMPエラーが出る場合の強制上書き
if(MINGW)
    target_link_libraries(BlackMidiPlayer PRIVATE gomp)
    target_compile_options(BlackMidiPlayer PRIVATE
            -fopenmp
            -static
            -static-libstdc++
            -static-libgcc)
endif()

# --- 6. ポストビルド (シェーダーファイルのコピー) ---
foreach(SHADER ${SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    add_custom_command(
            TARGET BlackMidiPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
            $<TARGET_FILE_DIR:BlackMidiPlayer>/${SHADER_NAME}
    )
endforeach()
