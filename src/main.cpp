#include "MainWindow.h"
#include "MdiIcons.h"
#include "ThemeManager.h"
#include <QApplication>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QIcon>
#include <QTimer>
class Application : public QApplication {
public:
  using QApplication::QApplication;
  MainWindow *window = nullptr;
  QString pending;
  bool event(QEvent *e) override {
    if (e->type() == QEvent::FileOpen) {
      auto file = static_cast<QFileOpenEvent *>(e)->file();
      if (window)
        window->openFile(file);
      else
        pending = file;
      return true;
    }
    return QApplication::event(e);
  }
};
int main(int argc, char **argv) {
  Application app(argc, argv);
  app.setOrganizationName("nex8er");
  app.setApplicationName("Tailor");
  app.setApplicationVersion("0.1.0");
  app.setWindowIcon(QIcon(":/tailor/icons/tailor.png"));
  spotty::ThemeManager theme;
  theme.setTheme(spotty::ThemeManager::Theme::Dark);
  spotty::MdiIcons::initialize();
  MainWindow window;
  app.window = &window;
  window.show();
  if (!app.pending.isEmpty())
    window.openFile(app.pending);
  const auto args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    if (args[i] == "--screenshot" && i + 1 < args.size()) {
      const auto path = args[++i];
      QTimer::singleShot(1800, &window, [&window, path] {
        window.grab().save(path);
        qApp->quit();
      });
    } else if (QFileInfo(args[i]).isFile())
      window.openFile(args[i]);
  }
  return app.exec();
}
