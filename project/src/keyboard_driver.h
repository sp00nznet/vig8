// vig8 -- Keyboard-to-gamepad input driver
// Maps keyboard keys to Xbox 360 controller inputs for slot 0.
// Registered via InsertDriverFront so it is queried before the SDL driver.
// Physical Xbox controller is merged via dynamically-loaded XInput.
//
// Key mapping:
//   WASD / Arrow keys  = D-pad (movement / menu navigation)
//   IJKL               = Right stick (turret)
//   Enter              = Start (pause)
//   Space / Z          = A (confirm / fire)
//   Escape / Backspace = B (back / cancel)
//   X                  = X button
//   C                  = Y button
//   Q / E              = LB / RB (shoulder buttons)
//   [ / ]              = LT / RT (analog triggers)
//   `                  = Back

#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <cstdint>

using rex::X_STATUS;
using rex::X_RESULT;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;
using rex::input::X_INPUT_KEYSTROKE;

class KeyboardInputDriver final : public rex::input::InputDriver,
                                  public rex::ui::WindowInputListener {
public:
    explicit KeyboardInputDriver(rex::ui::Window* window);
    ~KeyboardInputDriver() override;

    X_STATUS Setup() override;

    X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                             X_INPUT_CAPABILITIES* out_caps) override;
    X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
    X_RESULT SetState(uint32_t user_index,
                      X_INPUT_VIBRATION* vibration) override;
    X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                          X_INPUT_KEYSTROKE* out_keystroke) override;

    void OnKeyDown(rex::ui::KeyEvent& e) override {}
    void OnKeyUp(rex::ui::KeyEvent& e) override {}

private:
    uint32_t packet_number_ = 0;
    uint16_t prev_buttons_  = 0;
};
