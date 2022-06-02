#ifndef YODYPUSHBUTTON_H
#define YODYPUSHBUTTON_H
#include <QPushButton>
#include <QStyleOptionButton>
#include <QIcon>

class YodyPushButton : public QPushButton
{
public:
    explicit YodyPushButton(QWidget * parent = Q_NULLPTR);
    explicit YodyPushButton(const QString &text, QWidget *parent = Q_NULLPTR);

protected:
    void paintEvent(QPaintEvent *) Q_DECL_OVERRIDE;

private:
    void updateIcon(QStyleOptionButton &pushbutton);

private:
    bool m_iconCached;
    QIcon m_downIcon;
};

#endif // YODYPUSHBUTTON_H
