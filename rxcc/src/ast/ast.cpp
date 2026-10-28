#include "ast/ast.hpp"
#include <cassert>

namespace rx {

// 路径段的打印：段名 + 可选泛型（dump 与 Type::name 共用）
std::string to_string(const PathSegment &s) {
    std::string out = s.name;
    if (!s.generics.empty()) {
        out += "<";
        for (size_t i = 0; i < s.generics.size(); ++i) {
            if (i)
                out += ", ";
            assert(s.generics[i]);
            out += s.generics[i]->name();
        }
        out += ">";
    }
    return out;
}

std::string to_string(const std::vector<PathSegment> &segs) {
    std::string out;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i)
            out += "::";
        out += to_string(segs[i]);
    }
    return out;
}

namespace {

// 将直接子节点交给外部 visitor。visitor 可再次调用 walk 来递归遍历。
// 此处定义静态访问顺序，不模拟条件选择、循环次数或短路求值。
struct ChildWalker : AstVisitor {
    AstVisitor &out;
    explicit ChildWalker(AstVisitor &out) : out(out) {}

    void visit(IntegerLiteral &) override {} // 叶子：无孩子

    void visit(BinaryExpression &e) override {
        e.left->accept(out); // 普通操作数：从左到右
        e.right->accept(out);
    }

    void visit(BooleanLiteral &) override {}
    void visit(UnitExpression &) override {}
    void visit(PathExpression &) override {}

    void visit(UnaryExpression &e) override { e.operand->accept(out); }

    void visit(AssignmentExpression &e) override {
        e.right->accept(out);
        e.left->accept(
            out); // 普通赋值与复合赋值在此都按 right、left 的顺序遍历。
    }

    void visit(CastExpression &e) override {
        e.operand->accept(out); // target 是 Type 不是 Node，walk 不下钻
    }

    void visit(CallExpression &e) override {
        e.callee->accept(out); // callee 先
        for (auto &a : e.arguments)
            a->accept(out); // 参数从左到右
    }

    void visit(MethodCallExpression &e) override {
        e.receiver->accept(out); // receiver 先；method 段非 Node 不下钻
        for (auto &a : e.arguments)
            a->accept(out);
    }

    void visit(FieldExpression &e) override { e.base->accept(out); }

    void visit(IndexExpression &e) override {
        e.base->accept(out);  // base 先
        e.index->accept(out); // 下标后
    }

    void visit(BlockExpression &e) override {
        for (auto &s : e.statements)
            s->accept(out); // 语句按源码顺序
        if (e.tail)
            e.tail->accept(out); // 先遍历语句，最后访问尾表达式。
    }

    void visit(LetStatement &e) override {
        e.initializer->accept(out); // 唯一的 Node 孩子；name/is_mut/标注都不是
    }

    void visit(ExpressionStatement &e) override { e.expression->accept(out); }

    void visit(IfExpression &e) override {
        e.condition->accept(out); // 条件先
        e.then->accept(out);      // then 次之
        if (e.else_)
            e.else_->accept(out); // else 最后（可空）
    }

    void visit(LoopExpression &e) override { e.body->accept(out); }

    void visit(WhileExpression &e) override {
        e.condition->accept(out); // 静态遍历先访问条件，再访问循环体，各一次。
        e.body->accept(out);
    }

    void visit(BreakExpression &e) override {
        if (e.value)
            e.value->accept(out);
    }

    void visit(ContinueExpression &) override {}

    void visit(ReturnExpression &e) override {
        if (e.value)
            e.value->accept(out);
    }

    void visit(ArrayExpression &e) override {
        if (e.repeat_value) {
            e.repeat_value->accept(out); // 重复形只访问元素表达式一次，不按重复次数展开。
        } else {
            for (auto &el : e.elements)
                el->accept(out); // 列表形从左到右
        }
    }

    void visit(StructExpression &e) override {
        e.path->accept(out); // 先访问结构体路径，再按源码顺序访问字段值。
        for (auto &f : e.fields)
            f.value->accept(out); // 字段值按源代码顺序
    }

    void visit(Function &e) override { e.body->accept(out); } // 唯一 Node 孩子

    void visit(Struct &) override {
    } // 字段类型是 Type、derives 是枚举——都不下钻

    void visit(Const &) override {} // 类型和 ConstantValue 都不是 Node，不交给此 visitor。

    void visit(InherentImpl &e) override {
        for (auto &i : e.items)
            i->accept(out);
    }
};

} // namespace

void walk(Node &node, AstVisitor &visitor) {
    ChildWalker w{visitor};
    node.accept(w);
}

const char *name(BinaryOperator operation) {
    switch (operation) {
        case BinaryOperator::Add:
            return "Add";
        case BinaryOperator::Subtract:
            return "Sub";
        case BinaryOperator::Multiply:
            return "Mul";
        case BinaryOperator::Divide:
            return "Div";
        case BinaryOperator::Remainder:
            return "Rem";
        case BinaryOperator::BitwiseAnd:
            return "BitAnd";
        case BinaryOperator::BitwiseXor:
            return "BitXor";
        case BinaryOperator::BitwiseOr:
            return "BitOr";
        case BinaryOperator::ShiftLeft:
            return "Shl";
        case BinaryOperator::ShiftRight:
            return "Shr";
        case BinaryOperator::Equal:
            return "Eq";
        case BinaryOperator::NotEqual:
            return "Ne";
        case BinaryOperator::LessThan:
            return "Lt";
        case BinaryOperator::LessThanOrEqual:
            return "Le";
        case BinaryOperator::GreaterThan:
            return "Gt";
        case BinaryOperator::GreaterThanOrEqual:
            return "Ge";
        case BinaryOperator::LogicalAnd:
            return "LogAnd";
        case BinaryOperator::LogicalOr:
            return "LogOr";
    }
    return "";
}

const char *name(UnaryOperator operation) {
    switch (operation) {
        case UnaryOperator::Negative:
            return "Neg";
        case UnaryOperator::Not:
            return "Not";
        case UnaryOperator::Dereference:
            return "Deref";
        case UnaryOperator::Borrow:
            return "Borrow";
        case UnaryOperator::MutableBorrow:
            return "BorrowMut";
    }
    return "";
}

const char *name(AssignmentOperator operation) {
    switch (operation) {
        case AssignmentOperator::Assign:
            return "Assign";
        case AssignmentOperator::AddAssignment:
            return "AddAssign";
        case AssignmentOperator::SubtractAssignment:
            return "SubAssign";
        case AssignmentOperator::MultiplyAssignment:
            return "MulAssign";
        case AssignmentOperator::DivideAssignment:
            return "DivAssign";
        case AssignmentOperator::RemainderAssignment:
            return "RemAssign";
        case AssignmentOperator::BitwiseAndAssignment:
            return "BitAndAssign";
        case AssignmentOperator::BitwiseXorAssignment:
            return "BitXorAssign";
        case AssignmentOperator::BitwiseOrAssignment:
            return "BitOrAssign";
        case AssignmentOperator::ShiftLeftAssignment:
            return "ShlAssign";
        case AssignmentOperator::ShiftRightAssignment:
            return "ShrAssign";
    }
    return "";
}

std::string to_string(const ConstantValue &v) {
    switch (v.kind) {
        case ConstantValue::Kind::Int:
            return std::to_string(v.int_value);
        case ConstantValue::Kind::Bool:
            return v.bool_value ? "true" : "false";
        case ConstantValue::Kind::Path:
            return to_string(v.path);
        case ConstantValue::Kind::Negative:
            assert(v.operand); // Negative 的操作数在完整树中必非空
            return "-" + to_string(*v.operand);
    }
    return "";
}

const char *name(Derive d) {
    switch (d) {
        case Derive::Copy:
            return "Copy";
        case Derive::Clone:
            return "Clone";
        case Derive::PartialEq:
            return "PartialEq";
        case Derive::Eq:
            return "Eq";
    }
    return "";
}

const char *name(SelfParameter::Kind k) {
    switch (k) {
        case SelfParameter::Kind::Value:
            return "self";
        case SelfParameter::Kind::MutableValue:
            return "mut self";
        case SelfParameter::Kind::Reference:
            return "&self";
        case SelfParameter::Kind::MutableReference:
            return "&mut self";
    }
    return "";
}

// 语法类型名称供 AST dump 和诊断使用。
std::string PathType::name() const {
    return to_string(segments);
}

std::string ReferenceType::name() const {
    assert(inner);
    return is_mut ? "&mut " + inner->name() : "&" + inner->name();
}

std::string ArrayType::name() const {
    assert(element);
    return "[" + element->name() + "; " + to_string(length) + "]";
}

} // namespace rx
