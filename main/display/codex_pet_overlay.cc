#include "display/codex_pet_overlay.h"
#include "assets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cJSON.h>
#include <cstring>

static const char* TAG = "CodexPetOverlay";

// Emotion / device-state -> animation name. Uses the 9 standard Codex rows.
// Unknown inputs fall back to "idle".
static const struct {
  const char* input;
  const char* animation;
} kEmotionMap[] = {
    // calm / listening
    {"idle", "idle"},
    {"neutral", "idle"},
    {"listening", "idle"},
    {"sleepy", "idle"},
    {"relaxed", "idle"},
    // happy reactions
    {"happy", "waving"},
    {"laughing", "waving"},
    {"funny", "waving"},
    {"loving", "waving"},
    {"delicious", "waving"},
    {"kissy", "waving"},
    {"confident", "waving"},
    {"silly", "jumping"},
    {"excited", "jumping"},
    {"joy", "jumping"},
    {"surprised", "jumping"},
    {"shocked", "jumping"},
    // thinking / speaking (active)
    {"thinking", "waiting"},
    {"confused", "waiting"},
    {"speaking", "running"},
    // negative
    {"sad", "failed"},
    {"crying", "failed"},
    {"angry", "failed"},
    {"embarrassed", "failed"},
    {nullptr, nullptr}
};

CodexPetOverlay::CodexPetOverlay(lv_obj_t* parent_screen)
    : parent_screen_(parent_screen),
      pet_container_(nullptr),
      pet_image_(nullptr),
      visible_(false) {

  // Full-screen 480x480 transparent overlay container.
  pet_container_ = lv_obj_create(parent_screen_);
  lv_obj_set_size(pet_container_, 480, 480);
  lv_obj_set_pos(pet_container_, 0, 0);
  lv_obj_set_style_bg_opa(pet_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pet_container_, 0, 0);
  lv_obj_set_style_pad_all(pet_container_, 0, 0);
  lv_obj_clear_flag(pet_container_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(pet_container_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(pet_container_);

  // Image widget for the pet. Source is a per-frame descriptor set in AdvanceFrame.
  pet_image_ = lv_image_create(pet_container_);
  lv_obj_align(pet_image_, LV_ALIGN_CENTER, 0, 0);

  // Poll timer (LVGL thread) that services KEY3 test presses safely.
  test_poll_timer_ = lv_timer_create(TestPollCb, 50, this);

  ESP_LOGI(TAG, "CodexPetOverlay created (assets-partition frame animations)");
}

CodexPetOverlay::~CodexPetOverlay() {
  if (test_poll_timer_) {
    lv_timer_delete(test_poll_timer_);
    test_poll_timer_ = nullptr;
  }
  if (anim_timer_) {
    lv_timer_delete(anim_timer_);
    anim_timer_ = nullptr;
  }
  if (pet_container_) {
    lv_obj_del(pet_container_);
  }
}

bool CodexPetOverlay::LoadPetFromAssets() {
  animations_.clear();

  auto& assets = Assets::GetInstance();
  void* ptr = nullptr;
  size_t size = 0;
  if (!assets.GetAssetData("pet_manifest.json", ptr, size)) {
    ESP_LOGE(TAG, "pet_manifest.json not found in assets partition");
    return false;
  }

  cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
  if (!root) {
    ESP_LOGE(TAG, "pet_manifest.json invalid");
    return false;
  }

  // Pet identity comes from the manifest (display/logging only).
  cJSON* petname = cJSON_GetObjectItem(root, "pet");
  current_pet_ = cJSON_IsString(petname) ? petname->valuestring : "";

  cJSON* anims = cJSON_GetObjectItem(root, "animations");
  int n = cJSON_IsArray(anims) ? cJSON_GetArraySize(anims) : 0;
  animations_.reserve(n);

  for (int i = 0; i < n; i++) {
    cJSON* a = cJSON_GetArrayItem(anims, i);
    cJSON* jn = cJSON_GetObjectItem(a, "name");
    cJSON* ji = cJSON_GetObjectItem(a, "interval_ms");
    cJSON* jf = cJSON_GetObjectItem(a, "frames");
    if (!cJSON_IsString(jn) || !cJSON_IsNumber(jf)) continue;

    Animation anim;
    anim.name = jn->valuestring;
    anim.interval_ms = cJSON_IsNumber(ji) ? (uint16_t)ji->valueint : 180;
    int frames = jf->valueint;
    anim.frames.reserve(frames);

    for (int f = 0; f < frames; f++) {
      std::string fname = anim.name + "_" + std::to_string(f);
      void* fp = nullptr;
      size_t fs = 0;
      if (!assets.GetAssetData(fname, fp, fs) || fs < 12) {
        ESP_LOGW(TAG, "frame %s missing", fname.c_str());
        continue;
      }
      // Parse the 12-byte LVGL image header serialized ahead of the pixel map.
      const uint8_t* p = static_cast<const uint8_t*>(fp);
      lv_image_dsc_t dsc;
      memset(&dsc, 0, sizeof(dsc));
      dsc.header.magic = p[0];
      dsc.header.cf = p[1];
      dsc.header.flags = (uint16_t)(p[2] | (p[3] << 8));
      dsc.header.w = (uint16_t)(p[4] | (p[5] << 8));
      dsc.header.h = (uint16_t)(p[6] | (p[7] << 8));
      dsc.header.stride = (uint16_t)(p[8] | (p[9] << 8));
      dsc.data = p + 12;
      dsc.data_size = fs - 12;
      anim.frames.push_back(dsc);
    }
    if (!anim.frames.empty()) {
      animations_.push_back(std::move(anim));
    }
  }
  cJSON_Delete(root);

  ESP_LOGI(TAG, "Pet '%s' loaded: %d animations", current_pet_.c_str(),
           (int)animations_.size());
  if (animations_.empty()) return false;

  SetEmotion("idle");
  return true;
}

CodexPetOverlay::Animation* CodexPetOverlay::FindAnimation(const std::string& name) {
  for (auto& a : animations_) {
    if (a.name == name) return &a;
  }
  return nullptr;
}

void CodexPetOverlay::AdvanceFrame() {
  if (!current_anim_ || current_anim_->frames.empty()) return;
  const lv_image_dsc_t* frame = &current_anim_->frames[current_frame_];

  lv_image_set_src(pet_image_, frame);
  lv_obj_set_size(pet_image_, frame->header.w, frame->header.h);
  lv_obj_align(pet_image_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_invalidate(pet_image_);

  current_frame_++;
  if (current_frame_ >= current_anim_->frames.size()) {
    current_frame_ = 0;
    if (target_loops_ > 0) {
      loops_done_++;
      if (loops_done_ >= target_loops_ && anim_timer_) {
        lv_timer_pause(anim_timer_);  // hold on last frame
      }
    }
  }
}

void CodexPetOverlay::AnimTimerCb(lv_timer_t* timer) {
  auto* self = static_cast<CodexPetOverlay*>(lv_timer_get_user_data(timer));
  if (self) self->AdvanceFrame();
}

// Caller must hold the LVGL lock (or run in the LVGL thread).
void CodexPetOverlay::PlayAnimationLocked(Animation* anim, uint8_t loops) {
  if (!anim || anim->frames.empty()) return;

  current_anim_ = anim;
  current_frame_ = 0;
  target_loops_ = loops;
  loops_done_ = 0;
  lv_obj_clear_flag(pet_image_, LV_OBJ_FLAG_HIDDEN);
  AdvanceFrame();  // show first frame immediately

  if (anim_timer_ == nullptr) {
    anim_timer_ = lv_timer_create(AnimTimerCb, anim->interval_ms, this);
  } else {
    lv_timer_set_period(anim_timer_, anim->interval_ms);
    lv_timer_resume(anim_timer_);
  }
}

// Call from a non-LVGL thread (e.g. the app task in SetEmotion).
void CodexPetOverlay::PlayAnimation(Animation* anim, uint8_t loops) {
  lv_lock();
  PlayAnimationLocked(anim, loops);
  lv_unlock();
}

void CodexPetOverlay::UpdatePetImage(const char* emotion) {
  if (!emotion) return;
  Animation* anim = FindAnimation(emotion);
  if (!anim) anim = FindAnimation("idle");
  if (!anim) {
    ESP_LOGW(TAG, "No animation for '%s'", emotion);
    return;
  }
  PlayAnimation(anim, 0);  // loop forever for state-driven playback
  ESP_LOGI(TAG, "Playing '%s' (%d frames @ %dms)", anim->name.c_str(),
           (int)anim->frames.size(), anim->interval_ms);
  current_emotion_ = emotion;
}

const char* CodexPetOverlay::NormalizeEmotion(const char* emotion) {
  if (!emotion) return "idle";
  for (int i = 0; kEmotionMap[i].input != nullptr; i++) {
    if (strcmp(emotion, kEmotionMap[i].input) == 0) {
      return kEmotionMap[i].animation;
    }
  }
  return "idle";
}

void CodexPetOverlay::SetEmotion(const char* emotion) {
  const char* normalized = NormalizeEmotion(emotion);
  if (current_emotion_ == normalized) {
    return;  // No change
  }
  UpdatePetImage(normalized);
}

// Called from the button (esp_timer) context. Must be trivial and touch no
// LVGL state: only raise a flag. The poll timer (LVGL thread) does the work.
void CodexPetOverlay::CyclePlayForTest() {
  cycle_pending_.store(true);
}

void CodexPetOverlay::TestPollCb(lv_timer_t* timer) {
  auto* self = static_cast<CodexPetOverlay*>(lv_timer_get_user_data(timer));
  if (self && self->cycle_pending_.exchange(false)) {
    self->DoCycle();
  }
}

// Runs in the LVGL thread (from TestPollCb), so no lv_lock is needed here.
void CodexPetOverlay::DoCycle() {
  if (animations_.empty()) return;
  test_index_ = (test_index_ + 1) % (int)animations_.size();
  Animation* anim = &animations_[test_index_];

  PlayAnimationLocked(anim, 0);  // loop forever until the next press

  ESP_LOGI(TAG, "TEST: [%d/%d] loop '%s'", test_index_ + 1,
           (int)animations_.size(), anim->name.c_str());
}

void CodexPetOverlay::Show() {
  if (!pet_container_) return;
  lv_obj_clear_flag(pet_container_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(pet_container_);
  lv_obj_move_to_index(pet_container_, -1);
  visible_ = true;
  lv_obj_invalidate(pet_container_);
  lv_refr_now(lv_display_get_default());

  // Keep the pet on top if other UI is created later.
  static lv_timer_t* keep_foreground_timer = nullptr;
  if (keep_foreground_timer == nullptr) {
    keep_foreground_timer = lv_timer_create([](lv_timer_t* timer) {
      auto* container = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
      if (container && lv_obj_is_valid(container)) {
        lv_obj_move_foreground(container);
      }
    }, 1000, pet_container_);
  }
  ESP_LOGI(TAG, "Pet overlay shown");
}

void CodexPetOverlay::Hide() {
  if (pet_container_) {
    lv_obj_add_flag(pet_container_, LV_OBJ_FLAG_HIDDEN);
    visible_ = false;
  }
}
