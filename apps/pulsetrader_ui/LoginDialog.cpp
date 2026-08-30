// login_dialog.cpp — implementation of the login screen.

#include "LoginDialog.hpp"

#include "CredentialsStore.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QVBoxLayout>

#include <thread>

namespace pulse::ui
{

namespace
{

// Localised product-group names for the permission summary.
QString permissionDisplayName(const QString &rawName)
{
    if ("spot" == rawName)
    {
        return QStringLiteral("现货");
    }
    if ("futures" == rawName)
    {
        return QStringLiteral("合约（永续）");
    }
    if ("delivery" == rawName)
    {
        return QStringLiteral("合约（交割）");
    }
    if ("tradfi" == rawName)
    {
        return QStringLiteral("CFD");
    }
    return rawName;
}

// Render one permission entry: 未开通 / 只读 / 读写 / 无法判定.
QString permissionText(const PermissionInfo &p)
{
    if (false == p.available)
    {
        return QStringLiteral("未开通");
    }
    if (false == p.known)
    {
        return QStringLiteral("读写未知");
    }
    return p.readOnly ? QStringLiteral("只读") : QStringLiteral("读写");
}

} // namespace

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("模拟 Gate 系统 - 登录"));
    setMinimumWidth(420);

    // -----------------------------------------------------------------------
    // Layout: form fields + error line + action buttons.
    // -----------------------------------------------------------------------
    auto *form = new QFormLayout;
    m_keyEdit = new QLineEdit(this);
    m_keyEdit->setPlaceholderText(QStringLiteral("Gate.io API Key"));
    m_keyEdit->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("API Key"), m_keyEdit);

    m_secretEdit = new QLineEdit(this);
    m_secretEdit->setPlaceholderText(QStringLiteral("Gate.io API Secret"));
    m_secretEdit->setEchoMode(QLineEdit::Password);
    m_secretEdit->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("API Secret"), m_secretEdit);

    m_rememberCheck = new QCheckBox(QStringLiteral("记住凭证（下次自动填入，保存到 .env）"), this);
    m_rememberCheck->setChecked(true);
    form->addRow(QString(), m_rememberCheck);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #c0392b;"));
    m_errorLabel->hide();
    form->addRow(QString(), m_errorLabel);

    auto *buttons = new QHBoxLayout;
    m_cancelButton = new QPushButton(QStringLiteral("退出"), this);
    m_loginButton = new QPushButton(QStringLiteral("登录"), this);
    m_loginButton->setDefault(true);
    buttons->addStretch(1);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_loginButton);

    auto *root = new QVBoxLayout(this);
    auto *hint = new QLabel(
        QStringLiteral("登录后将从 Gate.io 拉取账户余额与黄金价格以校验凭证，"
                       "并进入引擎控制界面。"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #666;"));
    root->addWidget(hint);
    root->addLayout(form);
    root->addLayout(buttons);

    // -----------------------------------------------------------------------
    // Pre-fill from the remembered store.
    // -----------------------------------------------------------------------
    const QString envPath = CredentialsStore::resolveEnvPath();
    const StoredCredentials remembered = CredentialsStore::loadFromEnvFile(envPath);
    if (false == remembered.apiKey.isEmpty())
    {
        m_keyEdit->setText(remembered.apiKey);
    }
    if (false == remembered.apiSecret.isEmpty())
    {
        m_secretEdit->setText(remembered.apiSecret);
    }

    connect(m_loginButton, &QPushButton::clicked, this, &LoginDialog::onLoginClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &LoginDialog::onCancelClicked);
}

QString LoginDialog::apiKey() const
{
    return m_keyEdit->text().trimmed();
}

QString LoginDialog::apiSecret() const
{
    return m_secretEdit->text().trimmed();
}

const VerifyResult &LoginDialog::verifyResult() const
{
    return m_result;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void LoginDialog::onLoginClicked()
{
    const QString key = apiKey();
    const QString secret = apiSecret();
    if (key.isEmpty() || secret.isEmpty())
    {
        showError(QStringLiteral("请输入 API Key 和 API Secret。"));
        return;
    }

    m_errorLabel->hide();
    setBusy(true);

    // Background verification: the network round-trips (balance, gold price)
    // can take seconds; the UI must not freeze. The thread is detached and
    // reports back through a queued invocation guarded by QPointer — if the
    // dialog is closed meanwhile the callback is a no-op.
    QPointer<LoginDialog> self(this);
    const QString keyCopy = key;
    const QString secretCopy = secret;
    std::thread([keyCopy, secretCopy, self]() {
        const VerifyResult result = VerifyWorker::run(keyCopy, secretCopy);
        QMetaObject::invokeMethod(self, [self, result]() {
            if (self)
            {
                self->onVerifyFinished(result);
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void LoginDialog::onVerifyFinished(const VerifyResult &result)
{
    setBusy(false);

    if (false == result.ok)
    {
        showError(result.error);
        return;
    }

    m_result = result;

    // Persist the credentials before leaving the dialog.
    if (m_rememberCheck->isChecked())
    {
        const QString envPath = CredentialsStore::resolveEnvPath();
        if (false == CredentialsStore::saveToEnvFile(envPath, apiKey(), apiSecret()))
        {
            // Non-fatal: the engine can still be launched from memory.
            QMessageBox::warning(this, QStringLiteral("提示"),
                QStringLiteral("凭证校验通过，但写入 .env 失败（%1）。\n"
                               "本次会话仍可启动引擎，下次打开将不会自动填入。")
                    .arg(envPath));
        }
    }

    // Summary popup: account identity + balance + current gold price.
    QString summary = QStringLiteral("账户名称（Key）：%1\n\n").arg(result.maskedKey);
    summary += QStringLiteral("现货 USDT 可用余额：%1\n")
                   .arg(QString::number(result.spotUsdt, 'f', 2));
    if (result.cfdAvailable)
    {
        summary += QStringLiteral("CFD 账户权益：%1 USD\n")
                       .arg(QString::number(result.cfdEquity, 'f', 2));
    }
    else
    {
        summary += QStringLiteral("CFD 账户：不可用（未开通或无权限）\n");
    }
    if (result.goldAvailable)
    {
        summary += QStringLiteral("黄金现价（XAUUSD）：$%1\n")
                       .arg(QString::number(result.goldPrice, 'f', 2));
    }
    else
    {
        summary += QStringLiteral("黄金现价：获取失败\n");
    }
    if (false == result.permissions.empty())
    {
        summary += QStringLiteral("\nAPI 权限：\n");
        for (const auto &p : result.permissions)
        {
            summary += QStringLiteral("  %1：%2\n")
                           .arg(permissionDisplayName(p.name), permissionText(p));
        }
    }
    QMessageBox::information(this, QStringLiteral("登录成功"), summary);

    accept();
}

void LoginDialog::onCancelClicked()
{
    reject();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void LoginDialog::setBusy(bool busy)
{
    m_verifying = busy;
    m_loginButton->setEnabled(false == busy);
    m_cancelButton->setEnabled(false == busy);
    m_keyEdit->setEnabled(false == busy);
    m_secretEdit->setEnabled(false == busy);
    m_rememberCheck->setEnabled(false == busy);
    if (busy)
    {
        m_loginButton->setText(QStringLiteral("校验中…"));
    }
    else
    {
        m_loginButton->setText(QStringLiteral("登录"));
    }
}

void LoginDialog::showError(const QString &text)
{
    m_errorLabel->setText(text);
    m_errorLabel->show();
}

} // namespace pulse::ui
