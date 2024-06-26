/*
  This file is part of Knut.

  SPDX-FileCopyrightText: 2024 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>

  SPDX-License-Identifier: GPL-3.0-only

  Contact KDAB at <info@kdab.com> for commercial licensing options.
*/

#pragma once

#include "treesitter/node.h"
#include "treesitter/query.h"
#include "treesitter/tree.h"

#include <QAbstractItemModel>
#include <memory>
#include <optional>

namespace Gui {

class TreeSitterTreeModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    class TreeNode
    {
    public:
        explicit TreeNode(const treesitter::Node &node, const TreeNode *parent, bool enableUnnamed);

        int childCount() const;
        const TreeNode *child(int row) const;

        QVariant data(int column) const;
        int row() const;
        const TreeNode *parent() const;

        treesitter::Node tsNode() const;

        bool includesPosition(int position) const;

        const std::vector<std::unique_ptr<TreeNode>> &children() const;
        std::vector<std::unique_ptr<TreeNode>> &children();

        void traverse(
            const std::function<void(TreeNode *)> &fun, const std::function<bool(TreeNode *)> &filter = [](auto) {
                return true;
            });

    private:
        const TreeNode *m_parent;
        mutable std::vector<std::unique_ptr<TreeNode>> m_children;
        treesitter::Node m_node;
        bool m_enableUnnamed;
    };

    TreeSitterTreeModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QModelIndex indexFor(const TreeNode &node, int column) const;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &index) const override;

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void setQuery(const std::shared_ptr<treesitter::Query> &query,
                  std::unique_ptr<treesitter::Predicates> &&predicates);
    void setCursorPosition(int position);
    void setTree(treesitter::Tree &&tree, std::unique_ptr<treesitter::Predicates> &&predicates, bool enableUnnamed);
    void clear();

    std::optional<treesitter::Node> tsNode(const QModelIndex &index) const;

    bool hasQuery() const;
    int patternCount() const;
    int captureCount() const;
    int matchCount() const;

private:
    void positionChanged(int position);
    void capturesChanged(const std::unordered_map<treesitter::Node, QString> &oldCaptures);
    void executeQuery(std::unique_ptr<treesitter::Predicates> &&predicates);

    int m_cursorPosition;
    std::optional<treesitter::Tree> m_tree;

    struct QueryData
    {
        std::shared_ptr<treesitter::Query> query;
        std::unordered_map<treesitter::Node, QString> captures;
        int numMatches;
        int numCaptures;
    };

    std::optional<QueryData> m_query;
    std::unique_ptr<TreeNode> m_rootNode;
};

} // namespace Gui
