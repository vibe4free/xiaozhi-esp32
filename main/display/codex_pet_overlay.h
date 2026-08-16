#pragma once

#include "display/display.h"
#include "lvgl.h"
#include <atomic>
#include <string>
#include <vector>

// CodexPetOverlay draws an animated Codex pet on top of the base UI.
//
// Frames are loaded generically from the 'assets' partition (packed by
// scripts/pets/build_pet_assets.py) described by pet_manifest.json, so any
// Codex pet works without code changes. Each frame is an RGB565A8 image drawn
// directly from mmap'd flash (no decode buffer; the ESP32-C6 has no PSRAM).
class CodexPetOverlay {
 public:
  explicit CodexPetOverlay(lv_obj_t* parent_screen);
  virtual ~CodexPetOverlay();

  // Load whatever pet is present in the assets partition. The pet identity and
  // its animations come entirely from pet_manifest.json (per-frame assets are
  // named "<animation>_<index>"), so no pet name is hard-coded in the firmware.
  bool LoadPetFromAssets();

  // Control pet display
  void SetEmotion(const char* emotion);
  void Show();
  void Hide();
  bool IsVisible() const { return visible_; }

  // White-box test: advance to the next animation, play it a fixed number of
  // times, and show its name at the bottom of the screen. Decoupled from the
  // AI interaction so pet behaviour can be validated with a hardware key.
  void CyclePlayForTest();

 private:
  struct Animation {
    std::string name;
    std::vector<lv_image_dsc_t> frames;  // descriptors point into mmap'd flash
    uint16_t interval_ms = 180;
  };

  void UpdatePetImage(const char* emotion);
  const char* NormalizeEmotion(const char* emotion);
  Animation* FindAnimation(const std::string& name);
  void PlayAnimation(Animation* anim, uint8_t loops = 0);        // acquires lv_lock
  void PlayAnimationLocked(Animation* anim, uint8_t loops = 0);  // caller holds lv_lock
  void AdvanceFrame();
  void DoCycle();  // runs in LVGL thread (from the poll timer)
  static void AnimTimerCb(lv_timer_t* timer);
  static void TestPollCb(lv_timer_t* timer);

  lv_obj_t* parent_screen_;
  lv_obj_t* pet_container_;
  lv_obj_t* pet_image_;

  std::string current_pet_;
  std::string current_emotion_;
  bool visible_;

  // Animations loaded from the assets partition.
  std::vector<Animation> animations_;

  // Playback state
  Animation* current_anim_ = nullptr;
  uint8_t current_frame_ = 0;
  uint8_t target_loops_ = 0;   // 0 = loop forever; N = stop after N loops
  uint8_t loops_done_ = 0;
  int test_index_ = -1;        // current animation index for the KEY3 test
  lv_timer_t* anim_timer_ = nullptr;

  // KEY3 white-box test: the button callback (esp_timer context) only sets this
  // flag; a poll timer running in the LVGL thread performs the actual switch so
  // no LVGL call / lv_lock ever happens in the button context (avoids stalling
  // the esp_timer task, which previously froze the system).
  std::atomic<bool> cycle_pending_{false};
  lv_timer_t* test_poll_timer_ = nullptr;
};
