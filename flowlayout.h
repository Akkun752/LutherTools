#ifndef FLOWLAYOUT_H
#define FLOWLAYOUT_H

// Layout qui aligne ses widgets horizontalement et passe à la ligne
// automatiquement quand la largeur disponible est dépassée — utilisé pour
// afficher les tags du stream sous forme de "chips" façon Twitch.
// Adapté de l'exemple officiel Qt "Flow Layout"
// (Qt Widgets Examples : Layouts/Flow Layout, licence BSD-3-Clause).

#include <QLayout>
#include <QRect>
#include <QStyle>

class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent, int margin = -1, int hSpacing = -1, int vSpacing = -1);
    explicit FlowLayout(int margin = -1, int hSpacing = -1, int vSpacing = -1);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int) const override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QLayoutItem *takeAt(int index) override;

private:
    int doLayout(const QRect &rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem *> itemList;
    int m_hSpace;
    int m_vSpace;
};

#endif // FLOWLAYOUT_H
