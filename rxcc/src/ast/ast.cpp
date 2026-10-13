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

// 按规范求值顺序把孩子的 accept 交给外部 visitor。
struct ChildWalker : AstVisitor {
    AstVisitor &out;
    explicit ChildWalker(AstVisitor &out) : out(out) {}

    void visit(IntLit &) override {} // 叶子：无孩子

    void visit(BinaryExpression &e) override {
        e.lhs->accept(out); // 普通操作数：从左到右
        e.rhs->accept(out);
    }

    void visit(BoolLit &) override {}
    void visit(UnitExpression &) override {}
    void visit(PathExpression &) override {}

    void visit(UnaryExpression &e) override { e.operand->accept(out); }

    void visit(AssignmentExpression &e) override {
        e.rhs->accept(out);
        e.lhs->accept(
            out); // walk 顺序 = 求值顺序，赋值不是"从左到右"的普通操作数
    }

    void visit(CastExpression &e) override {
        e.operand->accept(out); // target 是 Type 不是 Node，walk 不下钻
    }

    void visit(CallExpression &e) override {
        e.callee->accept(out); // callee 先
        for (auto &a : e.args)
            a->accept(out); // 参数从左到右
    }

    void visit(MethodCallExpression &e) override {
        e.receiver->accept(out); // receiver 先；method 段非 Node 不下钻
        for (auto &a : e.args)
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
            e.tail->accept(out); // 尾表达式最后（设计 2.2）
    }

    void visit(LetStatement &e) override {
        e.init->accept(out); // 唯一的 Node 孩子；name/is_mut/标注都不是
    }

    void visit(ExpressionStatement &e) override { e.expr->accept(out); }

    void visit(IfExpression &e) override {
        e.condition->accept(out); // 条件先
        e.then->accept(out);      // then 次之
        if (e.else_)
            e.else_->accept(out); // else 最后（可空）
    }

    void visit(LoopExpression &e) override { e.body->accept(out); }

    void visit(WhileExpression &e) override {
        e.condition->accept(out); // 条件在前：每次迭代先重求值它
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
            e.repeat_value->accept(out); // 重复形：elem 只求值一次
        } else {
            for (auto &el : e.elements)
                el->accept(out); // 列表形从左到右
        }
    }

    void visit(StructExpression &e) override {
        e.path->accept(out); // 路径先（res 槽位所在）
        for (auto &f : e.fields)
            f.value->accept(out); // 字段值按源代码顺序
    }

    void visit(Function &e) override { e.body->accept(out); } // 唯一 Node 孩子

    void visit(Struct &) override {
    } // 字段类型是 Type、derives 是枚举——都不下钻

    void visit(Const &) override {} // value 是 ConstValue 纯值——声明不"执行"

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

const char *name(BinOp op) {
    switch (op) {
        case BinOp::Add:
            return "Add";
        case BinOp::Sub:
            return "Sub";
        case BinOp::Mul:
            return "Mul";
        case BinOp::Div:
            return "Div";
        case BinOp::Rem:
            return "Rem";
        case BinOp::BitAnd:
            return "BitAnd";
        case BinOp::BitXor:
            return "BitXor";
        case BinOp::BitOr:
            return "BitOr";
        case BinOp::Shl:
            return "Shl";
        case BinOp::Shr:
            return "Shr";
        case BinOp::Eq:
            return "Eq";
        case BinOp::Ne:
            return "Ne";
        case BinOp::Lt:
            return "Lt";
        case BinOp::Le:
            return "Le";
        case BinOp::Gt:
            return "Gt";
        case BinOp::Ge:
            return "Ge";
        case BinOp::LogAnd:
            return "LogAnd";
        case BinOp::LogOr:
            return "LogOr";
    }
    return "";
}

const char *name(UnOp op) {
    switch (op) {
        case UnOp::Neg:
            return "Neg";
        case UnOp::Not:
            return "Not";
        case UnOp::Deref:
            return "Deref";
        case UnOp::Borrow:
            return "Borrow";
        case UnOp::BorrowMut:
            return "BorrowMut";
    }
    return "";
}

const char *name(AssignOp op) {
    switch (op) {
        case AssignOp::Assign:
            return "Assign";
        case AssignOp::AddAssign:
            return "AddAssign";
        case AssignOp::SubAssign:
            return "SubAssign";
        case AssignOp::MulAssign:
            return "MulAssign";
        case AssignOp::DivAssign:
            return "DivAssign";
        case AssignOp::RemAssign:
            return "RemAssign";
        case AssignOp::BitAndAssign:
            return "BitAndAssign";
        case AssignOp::BitXorAssign:
            return "BitXorAssign";
        case AssignOp::BitOrAssign:
            return "BitOrAssign";
        case AssignOp::ShlAssign:
            return "ShlAssign";
        case AssignOp::ShrAssign:
            return "ShrAssign";
    }
    return "";
}

std::string to_string(const ConstValue &v) {
    switch (v.kind) {
        case ConstValue::Kind::Int:
            return std::to_string(v.int_value);
        case ConstValue::Kind::Bool:
            return v.bool_value ? "true" : "false";
        case ConstValue::Kind::Path:
            return to_string(v.path);
        case ConstValue::Kind::Neg:
            assert(v.operand); // Neg 的操作数在完整树中必非空
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

const char *name(SelfParam::Kind k) {
    switch (k) {
        case SelfParam::Kind::Value:
            return "self";
        case SelfParam::Kind::MutValue:
            return "mut self";
        case SelfParam::Kind::Ref:
            return "&self";
        case SelfParam::Kind::RefMut:
            return "&mut self";
    }
    return "";
}

// —— Type 层次的 equals/name 实现 ——
namespace {

bool type_ptr_equals(const std::shared_ptr<Type> &a,
                     const std::shared_ptr<Type> &b) {
    assert(a && b);
    return a->equals(*b);
}

bool segments_equal(const std::vector<PathSegment> &a,
                    const std::vector<PathSegment> &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name)
            return false;
        if (a[i].generics.size() != b[i].generics.size())
            return false;
        for (size_t j = 0; j < a[i].generics.size(); ++j) {
            if (!type_ptr_equals(a[i].generics[j], b[i].generics[j]))
                return false;
        }
    }
    return true;
}

bool const_value_equals(const ConstValue &a, const ConstValue &b) {
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
        case ConstValue::Kind::Int:
            return a.int_value == b.int_value;
        case ConstValue::Kind::Bool:
            return a.bool_value == b.bool_value;
        case ConstValue::Kind::Path:
            return segments_equal(a.path, b.path);
        case ConstValue::Kind::Neg:
            assert(a.operand && b.operand);
            return const_value_equals(*a.operand, *b.operand);
    }
    return false;
}

} // namespace

bool UnitType::equals(const Type &other) const {
    return dynamic_cast<const UnitType *>(&other) != nullptr;
}

std::string PathType::name() const {
    return to_string(segments);
}

bool PathType::equals(const Type &other) const {
    auto p = dynamic_cast<const PathType *>(&other);
    return p && segments_equal(segments, p->segments);
}

std::string ReferenceType::name() const {
    assert(inner);
    return is_mut ? "&mut " + inner->name() : "&" + inner->name();
}

bool ReferenceType::equals(const Type &other) const {
    auto r = dynamic_cast<const ReferenceType *>(&other);
    return r && is_mut == r->is_mut && type_ptr_equals(inner, r->inner);
}

std::string ArrayType::name() const {
    assert(element);
    return "[" + element->name() + "; " + to_string(length) + "]";
}

bool ArrayType::equals(const Type &other) const {
    auto a = dynamic_cast<const ArrayType *>(&other);
    return a && type_ptr_equals(element, a->element) &&
           const_value_equals(length, a->length);
}

} // namespace rx