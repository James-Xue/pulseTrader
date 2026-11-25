#pragma once
// llm_overlay_dialog.hpp — non-modal Claude Code overlay viewer
//
// Opened from the LLM盘面 button. The dialog is deliberately non-modal so the
// engine, v10 runner and paper positions keep running. It reads the latest
// audit record written by MainWindow::pollLlmOverlay; it never sends orders.

#include <QDialog>
#include <QString>

class QLabel;
class QTimer;

namespace pulse::ui
{

class LlmOverlayDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit LlmOverlayDialog(QWidget *parent = nullptr);

    /// Re-read the latest audit record and update all labels.
    void reload();

  private:
    QString auditPath() const;

    QLabel *m_status = nullptr;
    QLabel *m_error = nullptr;
    QLabel *m_time = nullptr;
    QLabel *m_bias = nullptr;
    QLabel *m_confidence = nullptr;
    QLabel *m_regime = nullptr;
    QLabel *m_support = nullptr;
    QLabel *m_resistance = nullptr;
    QLabel *m_entry = nullptr;
    QLabel *m_stop = nullptr;
    QLabel *m_targets = nullptr;
    QLabel *m_risk = nullptr;
    QLabel *m_context = nullptr;
    QLabel *m_reason = nullptr;
    QTimer *m_timer = nullptr;
};

} // namespace pulse::ui
