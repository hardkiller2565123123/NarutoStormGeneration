// generations - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <memory>

#include <generations_init.h>
#include <rex/rex_app.h>

#include "native_game_menu.h"

class GenerationsApp : public rex::ReXApp {
public:
  using rex::ReXApp::ReXApp;
  ~GenerationsApp() override;

  static std::unique_ptr<rex::ui::WindowedApp>
  Create(rex::ui::WindowedAppContext &ctx) {
    return std::unique_ptr<GenerationsApp>(
        new GenerationsApp(ctx, "generations", PPCImageConfig));
  }

protected:
  bool SetupEnvironment() override;
  void OnPreSetup(rex::RuntimeConfig &config) override;
  void OnConfigurePaths(rex::PathConfig &paths) override;
  std::optional<rex::PathConfig>
  OnFinalizePaths(const rex::PathConfig &defaults,
                  std::function<void(rex::PathConfig)> resume) override;
  void OnPostSetup() override;
  void OnShutdown() override;
  void OnKeyDown(rex::ui::KeyEvent &event) override;
  bool EnableHostOverlays() const override { return true; }

private:
  std::unique_ptr<generations::NativeGameMenu> native_game_menu_;
};
