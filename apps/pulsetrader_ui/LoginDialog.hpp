#pragma once
// login_dialog.hpp — first screen of the launcher
//
// Flow:
//   1. Pre-fills API key/secret from the remembered .env store (if any)
//   2. "登录" starts a background VerifyWorker (network on a detached
//      std::thread — the UI stays responsive)
//   3. On success a summary popup shows account identity, balance and the
//      current gold price; the credentials are written to .env when the
//      "记住凭证" box is checked, then the dialog accepts
//   4. On failure the reason is shown inline without closing the dialog
//
// The worker thread only talks to this object through a QPointer-guarded
// queued invocation, so closing the dialog mid-verification is safe.

#include <QDialog>
#include <QPointer>

#include "VerifyWorker.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace pulse::ui
{

class LoginDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit LoginDialog(QWidget *parent = nullptr);

    /// Credentials the user submitted (non-empty after a successful login).
    QString apiKey() const;
    QString apiSecret() const;

    /// Full verification outcome (balance, gold price, account identity).
    const VerifyResult &verifyResult() const;

  private slots:
    void onLoginClicked();
    void onVerifyFinished(const VerifyResult &result);
    void onCancelClicked();

  private:
    void setBusy(bool busy);
    void showError(const QString &text);

    QLineEdit *m_keyEdit;
    QLineEdit *m_secretEdit;
    QCheckBox *m_rememberCheck;
    QLabel *m_errorLabel;
    QPushButton *m_loginButton;
    QPushButton *m_cancelButton;

    VerifyResult m_result;
    bool m_verifying = false;
};

} // namespace pulse::ui
