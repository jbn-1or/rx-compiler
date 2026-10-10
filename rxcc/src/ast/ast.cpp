#include "ast/ast.hpp"
#include <cassert>
#include <ostream>

namespace rx {

namespace {

// 路径段的打印：段名 + 可选 turbofish 泛型
std::string segment_to_string(const PathSegment& s) {
    std::string out = s.name;
    if (!s.generics.empty()) {
        out += "<";
        for (size_t i = 0; i < s.generics.size(); ++i) {
            if (i) out += ", ";
            assert(s.generics[i]);
            out += s.generics[i]->name();
        }
        out += ">";
    }
    return out;
}

std::string segments_to_string(const std::vector<PathSegment>& segs) {
    std::string out;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i) out += "::";
        out += segment_to_string(segs[i]);
    }
    return out;
}

} // namespace

namespace {

// 一行一节点，depth 控制缩进。对外只暴露 dump
struct Printer : AstVisitor {
    std::ostream& os;
    int depth = 0;
    explicit Printer(std::ostream& os) : os(os) {}

    void indent() { for (int i = 0; i < depth; ++i) os << "  "; }

    void visit(IntLit& e) override {
        indent();
        os << "IntLit " << e.value << "\n";
    }

    void visit(BinaryExpression& e) override {
        indent();
        os << "Binary " << name(e.op) << "\n";
        ++depth;          // 进孩子前加深缩进
        walk(e, *this);   // 继续访问孩子
        --depth;          // 出来还原
    }

    void visit(BoolLit& e) override {
        indent();  os << "BoolLit " << (e.value ? "true" : "false") << "\n";
    }

    void visit(UnitExpression&) override {
        indent();  os << "UnitExpression\n";
    }                 

    void visit(PathExpression& e) override {
        indent();  os << "Path " << segments_to_string(e.segments) << "\n";
    }

    void visit(UnaryExpression& e) override {
        indent();  os << "Unary " << name(e.op) << "\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(AssignmentExpression& e) override {
        indent();  os << "Assignment " << name(e.op) << "\n";
        ++depth;  walk(e, *this);  --depth;  // 孩子顺序由 walk 定：rhs 先（求值顺序）
    }

    void visit(CastExpression& e) override {
        indent();  os << "Cast -> " << e.target->name() << "\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(CallExpression& e) override {
        indent();  os << "Call\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(MethodCallExpression& e) override {
        indent();  os << "MethodCall " << segment_to_string(e.method) << "\n";  // 含 turbofish 泛型
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(FieldExpression& e) override {
        indent();  os << "Field " << e.field << "\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(IndexExpression& e) override {
        indent();  os << "Index\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(BlockExpression& e) override {
        indent();  os << "Block\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(LetStatement& e) override {
        // 标注省略时 type_annotation 为空，不输出 ": 类型"
        indent();  os << "Let " << (e.is_mut ? "mut " : "") << e.name;
        if (e.type_annotation) os << ": " << e.type_annotation->name();
        os << "\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(ExpressionStatement& e) override {
        indent();  os << "ExpressionStatement\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(IfExpression& e) override {
        indent();  os << "If\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(LoopExpression& e) override {
        indent();  os << "Loop\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(WhileExpression& e) override {
        indent();  os << "While\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(BreakExpression& e) override {
        indent();  os << "Break\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(ContinueExpression&) override {
        indent();  os << "Continue\n";
    }

    void visit(ReturnExpression& e) override {
        indent();  os << "Return\n";
        ++depth;  walk(e, *this);  --depth;
    }

    void visit(ArrayExpression& e) override {
        indent();
        if (e.repeat_value) os << "Array repeat " << to_string(e.repeat_count) << "\n";
        else                os << "Array\n";
        ++depth;  walk(e, *this);  --depth;
    }

    // Struct
    void visit(StructExpression& e) override {
        indent();  os << "Struct\n";
        ++depth;
        e.path->accept(*this);  // 路径一行
        for (auto& f : e.fields) {  // 每个 Field 一行 + 值缩进
            indent();  os << "Field " << f.name << "\n";
            ++depth;  f.value->accept(*this);  --depth;
        }
        --depth;
    }

    void visit(Function& e) override {
        // ret 非空约定：省略 ARROW 时构建器直接填 UnitType 实例，打印为 ()
        indent();  os << "Function " << e.name << " -> " << e.ret->name() << "\n";
        ++depth;
        if (e.self_param) {
            indent();  os << "Self " << name(e.self_param->kind) << "\n";
        }
        for (auto& p : e.params) {
            indent();  os << "Param " << (p.is_mut ? "mut " : "") << p.name
                          << ": " << p.type->name() << "\n";
        }
        walk(e, *this);
        --depth;
    }

    void visit(Struct& e) override {
        indent();  os << "Struct " << e.name << "\n";
        ++depth;
        for (auto d : e.derives) {
            indent();  os << "Derive " << name(d) << "\n";
        }
        for (auto& f : e.fields) {
            indent();  os << "Field " << f.name << ": " << f.type->name() << "\n";
        }
        --depth;
    }

    void visit(Const& e) override {
        indent();  os << "Const " << e.name << ": " << e.type->name()
                      << " = " << to_string(e.value) << "\n";
    }

    void visit(InherentImpl& e) override {
        indent();  os << "Impl " << e.self_type->name() << "\n";
        ++depth;  walk(e, *this);  --depth;
    }
};

// 按规范求值顺序把孩子的 accept 交给外部 visitor。
struct ChildWalker : AstVisitor {
    AstVisitor& out;
    explicit ChildWalker(AstVisitor& out) : out(out) {}

    void visit(IntLit&) override {}  // 叶子：无孩子

    void visit(BinaryExpression& e) override {
        e.lhs->accept(out);  // 普通操作数：从左到右
        e.rhs->accept(out);
    }

    void visit(BoolLit&) override {}
    void visit(UnitExpression&) override {}
    void visit(PathExpression&) override {}
    
    void visit(UnaryExpression& e) override { e.operand->accept(out); }

    void visit(AssignmentExpression& e) override {
        e.rhs->accept(out);  // 规范 1746 行：普通赋值"先求值右侧值，再求值目标地址"
        e.lhs->accept(out);  // walk 顺序 = 求值顺序，赋值不是"从左到右"的普通操作数
    }

    void visit(CastExpression& e) override {
        e.operand->accept(out);  // target 是 Type 不是 Node，walk 不下钻
    }

    void visit(CallExpression& e) override {
        e.callee->accept(out);                  // callee 先
        for (auto& a : e.args) a->accept(out); // 参数从左到右（规范 1885）
    }

    void visit(MethodCallExpression& e) override {
        e.receiver->accept(out);                // receiver 先；method 段非 Node 不下钻
        for (auto& a : e.args) a->accept(out);
    }

    void visit(FieldExpression& e) override {
        e.base->accept(out);
    }

    void visit(IndexExpression& e) override {
        e.base->accept(out);   // base 先
        e.index->accept(out);  // 下标后
    }

    void visit(BlockExpression& e) override {
        for (auto& s : e.statements) s->accept(out);  // 语句按源码顺序
        if (e.tail) e.tail->accept(out);              // 尾表达式最后（设计 2.2）
    }

    void visit(LetStatement& e) override {
        e.init->accept(out);  // 唯一的 Node 孩子；name/is_mut/标注都不是
    }

    void visit(ExpressionStatement& e) override {
        e.expr->accept(out);
    }

    void visit(IfExpression& e) override {
        e.condition->accept(out);            // 条件先
        e.then->accept(out);                 // then 次之
        if (e.else_) e.else_->accept(out);   // else 最后（可空）
    }

    void visit(LoopExpression& e) override { e.body->accept(out); }

    void visit(WhileExpression& e) override {
        e.condition->accept(out);  // 条件在前：每次迭代先重求值它
        e.body->accept(out);
    }

    void visit(BreakExpression& e) override { if (e.value) e.value->accept(out); }

    void visit(ContinueExpression&) override {}

    void visit(ReturnExpression& e) override { if (e.value) e.value->accept(out); }

    void visit(ArrayExpression& e) override {
        if (e.repeat_value) {
            e.repeat_value->accept(out);  // 重复形：elem 只求值一次
        } else {
            for (auto& el : e.elements) el->accept(out);  // 列表形从左到右
        }
        // repeat_count 是 ConstValue 纯值：常量上下文求值，不是运行时，walk 不下钻
    }

    void visit(StructExpression& e) override {
        e.path->accept(out);                            // 路径先（res 槽位所在）
        for (auto& f : e.fields) f.value->accept(out);  // 字段值按源代码顺序
    }

    void visit(Function& e) override { e.body->accept(out); }  // 唯一 Node 孩子

    void visit(Struct&) override {}   // 字段类型是 Type、derives 是枚举——都不下钻

    void visit(Const&) override {}    // value 是 ConstValue 纯值——声明不"执行"

    void visit(InherentImpl& e) override {
        for (auto& i : e.items) i->accept(out);
    }
};

} // namespace

void walk(Node& node, AstVisitor& visitor) {
    ChildWalker w{visitor};
    node.accept(w);
}

void dump(Expression& expr, std::ostream& os) {
    Printer p{os};
    expr.accept(p);
}

void dump(Crate& crate, std::ostream& os) {
    Printer p{os};
    os << "Crate\n";
    ++p.depth;
    for (auto& item : crate.items) item->accept(p);
}

const char* name(BinOp op) {
    switch (op) {
        case BinOp::Add:    return "Add";
        case BinOp::Sub:    return "Sub";
        case BinOp::Mul:    return "Mul";
        case BinOp::Div:    return "Div";
        case BinOp::Rem:    return "Rem";
        case BinOp::BitAnd: return "BitAnd";
        case BinOp::BitXor: return "BitXor";
        case BinOp::BitOr:  return "BitOr";
        case BinOp::Shl:    return "Shl";
        case BinOp::Shr:    return "Shr";
        case BinOp::Eq:     return "Eq";
        case BinOp::Ne:     return "Ne";
        case BinOp::Lt:     return "Lt";
        case BinOp::Le:     return "Le";
        case BinOp::Gt:     return "Gt";
        case BinOp::Ge:     return "Ge";
        case BinOp::LogAnd: return "LogAnd";
        case BinOp::LogOr:  return "LogOr";
    }
    return "";
}

const char* name(UnOp op) {
    switch (op) {
        case UnOp::Neg:       return "Neg";
        case UnOp::Not:       return "Not";
        case UnOp::Deref:     return "Deref";
        case UnOp::Borrow:    return "Borrow";
        case UnOp::BorrowMut: return "BorrowMut";
    }
    return "";
}

const char* name(AssignOp op) {
    switch (op) {
        case AssignOp::Assign:       return "Assign";
        case AssignOp::AddAssign:    return "AddAssign";
        case AssignOp::SubAssign:    return "SubAssign";
        case AssignOp::MulAssign:    return "MulAssign";
        case AssignOp::DivAssign:    return "DivAssign";
        case AssignOp::RemAssign:    return "RemAssign";
        case AssignOp::BitAndAssign: return "BitAndAssign";
        case AssignOp::BitXorAssign: return "BitXorAssign";
        case AssignOp::BitOrAssign:  return "BitOrAssign";
        case AssignOp::ShlAssign:    return "ShlAssign";
        case AssignOp::ShrAssign:    return "ShrAssign";
    }
    return "";
}

std::string to_string(const ConstValue& v) {
    switch (v.kind) {
        case ConstValue::Kind::Int:  return std::to_string(v.int_value);
        case ConstValue::Kind::Bool: return v.bool_value ? "true" : "false";
        case ConstValue::Kind::Path: return segments_to_string(v.path);
        case ConstValue::Kind::Neg:
            assert(v.operand);   // Neg 的操作数在完整树中必非空
            return "-" + to_string(*v.operand);
    }
    return "";
}

const char* name(Derive d) {
    switch (d) {
        case Derive::Copy:      return "Copy";
        case Derive::Clone:     return "Clone";
        case Derive::PartialEq: return "PartialEq";
        case Derive::Eq:        return "Eq";
    }
    return "";
}

const char* name(SelfParam::Kind k) {
    switch (k) {
        case SelfParam::Kind::Value:    return "self";
        case SelfParam::Kind::MutValue: return "mut self";
        case SelfParam::Kind::Ref:      return "&self";
        case SelfParam::Kind::RefMut:   return "&mut self";
    }
    return "";
}

// —— Type 层次的 equals/name 实现 ——
namespace {

bool type_ptr_equals(const std::shared_ptr<Type>& a, const std::shared_ptr<Type>& b) {
    assert(a && b);
    return a->equals(*b);
}

bool segments_equal(const std::vector<PathSegment>& a, const std::vector<PathSegment>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name) return false;
        if (a[i].generics.size() != b[i].generics.size()) return false;
        for (size_t j = 0; j < a[i].generics.size(); ++j) {
            if (!type_ptr_equals(a[i].generics[j], b[i].generics[j])) return false;
        }
    }
    return true;
}

bool const_value_equals(const ConstValue& a, const ConstValue& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case ConstValue::Kind::Int:  return a.int_value == b.int_value;
        case ConstValue::Kind::Bool: return a.bool_value == b.bool_value;
        case ConstValue::Kind::Path: return segments_equal(a.path, b.path);
        case ConstValue::Kind::Neg:
            assert(a.operand && b.operand);
            return const_value_equals(*a.operand, *b.operand);
    }
    return false;
}

} // namespace

bool UnitType::equals(const Type& other) const {
    return dynamic_cast<const UnitType*>(&other) != nullptr;
}

std::string PathType::name() const {
    return segments_to_string(segments);
}

bool PathType::equals(const Type& other) const {
    auto p = dynamic_cast<const PathType*>(&other);
    return p && segments_equal(segments, p->segments);
}

std::string ReferenceType::name() const {
    assert(inner);
    return is_mut ? "&mut " + inner->name() : "&" + inner->name();
}

bool ReferenceType::equals(const Type& other) const {
    auto r = dynamic_cast<const ReferenceType*>(&other);
    return r && is_mut == r->is_mut && type_ptr_equals(inner, r->inner);
}

std::string ArrayType::name() const {
    assert(element);
    return "[" + element->name() + "; " + to_string(length) + "]";
}

bool ArrayType::equals(const Type& other) const {
    auto a = dynamic_cast<const ArrayType*>(&other);
    return a && type_ptr_equals(element, a->element) && const_value_equals(length, a->length);
}

} // namespace rx