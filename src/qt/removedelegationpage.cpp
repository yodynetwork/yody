#include "removedelegationpage.h"
#include "qt/forms/ui_removedelegationpage.h"

#include <validation.h>
#include <util/moneystr.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/rpcconsole.h>
#include <qt/bitcoinunits.h>
#include <qt/execrpccommand.h>
#include <qt/sendcoinsdialog.h>
#include <qt/hardwaresigntx.h>
#include <node/ui_interface.h>

namespace RemoveDelegation_NS
{
static const QString PRC_COMMAND = "removedelegationforaddress";
static const QString PARAM_ADDRESS = "address";
static const QString PARAM_GASLIMIT = "gaslimit";
static const QString PARAM_GASPRICE = "gasprice";

static const CAmount SINGLE_STEP = 0.00000001*COIN;
}
using namespace RemoveDelegation_NS;

RemoveDelegationPage::RemoveDelegationPage(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::RemoveDelegationPage),
    m_model(nullptr),
    m_clientModel(nullptr),
    m_execRPCCommand(nullptr)
{
    ui->setupUi(this);

    setWindowTitle(tr("Remove delegation"));

    ui->labelAddress->setToolTip(tr("Remove delegation for address."));

    // Set defaults
    ui->lineEditGasPrice->setValue(DEFAULT_GAS_PRICE);
    ui->lineEditGasPrice->setSingleStep(SINGLE_STEP);
    ui->lineEditGasLimit->setMinimum(MINIMUM_GAS_LIMIT);
    ui->lineEditGasLimit->setMaximum(DEFAULT_GAS_LIMIT_OP_SEND);
    ui->lineEditGasLimit->setValue(DEFAULT_GAS_LIMIT_OP_SEND);
    ui->lineEditAddress->setReadOnly(true);

    // Create new PRC command line interface
    QStringList lstMandatory;
    lstMandatory.append(PARAM_ADDRESS);

    QStringList lstOptional;
    lstOptional.append(PARAM_GASLIMIT);
    lstOptional.append(PARAM_GASPRICE);

    QMap<QString, QString> lstTranslations;
    lstTranslations[PARAM_ADDRESS] = ui->labelAddress->text();
    lstTranslations[PARAM_GASLIMIT] = ui->labelGasLimit->text();
    lstTranslations[PARAM_GASPRICE] = ui->labelGasPrice->text();

    m_execRPCCommand = new ExecRPCCommand(PRC_COMMAND, lstMandatory, lstOptional, lstTranslations, this);

    connect(ui->removeDelegationButton, &QPushButton::clicked, this, &RemoveDelegationPage::on_removeDelegationClicked);
    connect(ui->lineEditAddress, &QValidatedLineEdit::textChanged, this, &RemoveDelegationPage::on_updateRemoveDelegationButton);
}

RemoveDelegationPage::~RemoveDelegationPage()
{
    delete ui;
}

void RemoveDelegationPage::setModel(WalletModel *_model)
{
    m_model = _model;

    if (m_model && m_model->getOptionsModel())
        connect(m_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &RemoveDelegationPage::updateDisplayUnit);

    // update the display unit, to not use the default ("YODY")
    updateDisplayUnit();

    bCreateUnsigned = m_model->createUnsigned();

    if (bCreateUnsigned) {
        ui->removeDelegationButton->setText(tr("Cr&eate Unsigned"));
        ui->removeDelegationButton->setToolTip(tr("Creates a Partially Signed Yody Transaction (PSBT) for use with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
    }
}

void RemoveDelegationPage::setClientModel(ClientModel *_clientModel)
{
    m_clientModel = _clientModel;

    if (m_clientModel)
    {
        connect(m_clientModel, SIGNAL(gasInfoChanged(quint64, quint64, quint64)), this, SLOT(on_gasInfoChanged(quint64, quint64, quint64)));
    }
}

void RemoveDelegationPage::clearAll()
{
    ui->lineEditGasLimit->setValue(DEFAULT_GAS_LIMIT_OP_SEND);
    ui->lineEditGasPrice->setValue(DEFAULT_GAS_PRICE);
}

void RemoveDelegationPage::setDelegationData(const QString &_address, const QString &_hash)
{
    address = _address;
    hash = _hash;
    ui->lineEditAddress->setText(address);
}

bool RemoveDelegationPage::isDataValid()
{
    return !address.isEmpty() && !hash.isEmpty();
}

void RemoveDelegationPage::on_gasInfoChanged(quint64 blockGasLimit, quint64 minGasPrice, quint64 nGasPrice)
{
    Q_UNUSED(nGasPrice)
    ui->labelGasLimit->setToolTip(tr("Gas limit. Default = %1, Max = %2").arg(DEFAULT_GAS_LIMIT_OP_SEND).arg(blockGasLimit));
    ui->labelGasPrice->setToolTip(tr("Gas price: YODY price per gas unit. Default = %1, Min = %2").arg(QString::fromStdString(FormatMoney(DEFAULT_GAS_PRICE))).arg(QString::fromStdString(FormatMoney(minGasPrice))));
    ui->lineEditGasPrice->SetMinValue(minGasPrice);
    ui->lineEditGasLimit->setMaximum(blockGasLimit);
}

void RemoveDelegationPage::accept()
{
    clearAll();
    QDialog::accept();
}

void RemoveDelegationPage::reject()
{
    clearAll();
    QDialog::reject();
}

void RemoveDelegationPage::show()
{
    QDialog::show();
}

void RemoveDelegationPage::on_clearButton_clicked()
{
    reject();
}

void RemoveDelegationPage::on_removeDelegationClicked()
{
    if(m_model && m_clientModel)
    {
        if(!isDataValid())
            return;

        // Initialize variables
        QMap<QString, QString> lstParams;
        QVariant result;
        QString errorMessage;
        QString resultJson;
        int unit = BitcoinUnits::BTC;
        uint64_t gasLimit = ui->lineEditGasLimit->value();
        CAmount gasPrice = ui->lineEditGasPrice->value();
        std::string sDelegateAddress = address.toStdString();
        std::string sHash = hash.toStdString();

        // Get delegation details
        interfaces::DelegationDetails details = m_model->wallet().getDelegationDetails(sDelegateAddress);
        if(!details.c_contract_return)
        {
            QMessageBox::warning(this, tr("Remove delegation for address"), tr("Fail to get delegation details for the address."));
            return;
        }

        // Don't remove if in creating state
        if(details.w_create_exist && !details.w_create_abandoned &&
                (details.w_create_in_mempool || !details.w_create_in_main_chain))
        {
            QString txid = QString::fromStdString(details.w_create_tx_hash.ToString());
            QMessageBox::information(this, tr("Remove delegation for address"), tr("Please wait for the create contract delegation transaction be confirmed or abandon the transaction.\n\nTransaction ID: %1").arg(txid));
            return;
        }

        // Don't remove if in deleting state
        if(details.w_remove_exist && !details.w_remove_abandoned &&
                (details.w_remove_in_mempool || !details.w_remove_in_main_chain))
        {
            QString txid = QString::fromStdString(details.w_remove_tx_hash.ToString());
            QMessageBox::information(this, tr("Remove delegation for address"), tr("Please wait for the remove contract delegation transaction be confirmed or abandon the transaction.\n\nTransaction ID: %1").arg(txid));
            return;
        }

        // Check entry exist
        if(!details.c_entry_exist)
        {
            // Don't remove if chain is not synced to the specific block
            int numBlocks = m_clientModel->node().getNumBlocks();
            if(numBlocks <= 0 || numBlocks <= details.w_block_number)
                return;

            QMessageBox::information(this, tr("Remove delegation for address"), tr("Delegation already removed. \nThe delegation for the address will be removed from the wallet list."));
            m_model->wallet().removeDelegationEntry(sHash);
            accept();
            return;
        }

        // Unlock wallet
        WalletModel::UnlockContext ctx(m_model->requestUnlock());
        if(!ctx.isValid())
        {
            return;
        }

        // Append params to the list
        ExecRPCCommand::appendParam(lstParams, PARAM_ADDRESS, address);
        ExecRPCCommand::appendParam(lstParams, PARAM_GASLIMIT, QString::number(gasLimit));
        ExecRPCCommand::appendParam(lstParams, PARAM_GASPRICE, BitcoinUnits::format(unit, gasPrice, false, BitcoinUnits::SeparatorStyle::NEVER));

        QString questionString;
        if (bCreateUnsigned) {
            questionString.append(tr("Do you want to draft this transaction?"));
            questionString.append("<br /><span style='font-size:10pt;'>");
            questionString.append(tr("Please, review your transaction proposal. This will produce a Partially Signed Yody Transaction (PSBT) which you can copy and then sign with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
            questionString.append("</span>");
            questionString.append(tr("<br /><br />Remove delegation for address:<br />"));
        } else {
            questionString.append(tr("Are you sure you want to remove the delegation for the address: <br /><br />"));

        }
        questionString.append(tr("<b>%1</b>?")
                              .arg(ui->lineEditAddress->text()));

        const QString confirmation = bCreateUnsigned ? tr("Confirm remove delegation proposal.") : tr("Confirm remove delegation.");
        const QString confirmButtonText = bCreateUnsigned ? tr("Copy PSBT to clipboard") : tr("Send");
        SendConfirmationDialog confirmationDialog(confirmation, questionString, "", "", SEND_CONFIRM_DELAY, confirmButtonText, this);
        confirmationDialog.exec();

        QMessageBox::StandardButton retval = (QMessageBox::StandardButton)confirmationDialog.result();
        if(retval == QMessageBox::Yes)
        {
            // Execute RPC command line
            if(!m_execRPCCommand->exec(m_model->node(), m_model, lstParams, result, resultJson, errorMessage))
            {
                QMessageBox::warning(this, tr("Remove delegation for address"), errorMessage);
            }
            else
            {
                QVariantMap variantMap = result.toMap();
                if(bCreateUnsigned)
                {
                    GUIUtil::setClipboard(variantMap.value("psbt").toString());
                    Q_EMIT message(tr("PSBT copied"), "Copied to clipboard", CClientUIInterface::MSG_INFORMATION);
                }
                else
                {
                    bool isSent = true;
                    if(m_model->getSignPsbtWithHwiTool())
                    {
                        QString psbt = variantMap.value("psbt").toString();
                        if(!HardwareSignTx::process(this, m_model, psbt, variantMap))
                            isSent = false;
                    }

                    if(isSent)
                    {
                        std::string txid = variantMap.value("txid").toString().toStdString();
                        m_model->wallet().setDelegationRemoved(sHash, txid);
                    }
                }
            }

            accept();
        }
    }
}

void RemoveDelegationPage::on_updateRemoveDelegationButton()
{
    bool enabled = true;
    if(ui->lineEditAddress->text().isEmpty())
    {
        enabled = false;
    }

    ui->removeDelegationButton->setEnabled(enabled);
}

void RemoveDelegationPage::updateDisplayUnit()
{
    if(m_model && m_model->getOptionsModel())
    {
        // Update gasPriceAmount with the current unit
        ui->lineEditGasPrice->setDisplayUnit(m_model->getOptionsModel()->getDisplayUnit());
    }
}
