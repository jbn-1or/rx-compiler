#include "ast/dump.hpp"

#include <ostream>

namespace rx {

namespace {

// 一行一节点，depth 控制缩进。对外只暴露 dump
struct Printer : AstVisitor {
    std::ostream &os;
    int depth = 0;
    explicit Printer(std::ostream &os) : os(os) {}

    void indent() {
        for (int i = 0; i < depth; ++i)
            os << "  ";
    }

    void visit(IntLit &e) override {
        indent();
        os << "IntLit " << e.value << "\n";
    }

    void visit(BinaryExpression &e) override {
        indent();
        os << "Binary " << name(e.op) << "\n";
        ++depth;        // 进孩子前加深缩进
        walk(e, *this); // 继续访问孩子
        --depth;        // 出来还原
    }

    void visit(BoolLit &e) override {
        indent();
        os << "BoolLit " << (e.value ? "true" : "false") << "\n";
    }

    void visit(UnitExpression &) override {
        indent();
        os << "UnitExpression\n";
    }

    void visit(PathExpression &e) override {
        indent();
        os << "Path " << to_string(e.segments) << "\n";
    }

    void visit(UnaryExpression &e) override {
        indent();
        os << "Unary " << name(e.op) << "\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(AssignmentExpression &e) override {
        indent();
        os << "Assignment " << name(e.op) << "\n";
        ++depth;
        walk(e, *this);
        --depth; // 孩子顺序由 walk 定：rhs 先（求值顺序）
    }

    void visit(CastExpression &e) override {
        indent();
        os << "Cast -> " << e.target->name() << "\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(CallExpression &e) override {
        indent();
        os << "Call\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(MethodCallExpression &e) override {
        indent();
        os << "MethodCall " << to_string(e.method)
           << "\n"; // 含 turbofish 泛型
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(FieldExpression &e) override {
        indent();
        os << "Field " << e.field << "\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(IndexExpression &e) override {
        indent();
        os << "Index\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(BlockExpression &e) override {
        indent();
        os << "Block\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(LetStatement &e) override {
        // 标注省略时 type_annotation 为空，不输出 ": 类型"
        indent();
        os << "Let " << (e.is_mut ? "mut " : "") << e.name;
        if (e.type_annotation)
            os << ": " << e.type_annotation->name();
        os << "\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(ExpressionStatement &e) override {
        indent();
        os << "ExpressionStatement\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(IfExpression &e) override {
        indent();
        os << "If\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(LoopExpression &e) override {
        indent();
        os << "Loop\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(WhileExpression &e) override {
        indent();
        os << "While\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(BreakExpression &e) override {
        indent();
        os << "Break\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(ContinueExpression &) override {
        indent();
        os << "Continue\n";
    }

    void visit(ReturnExpression &e) override {
        indent();
        os << "Return\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    void visit(ArrayExpression &e) override {
        indent();
        if (e.repeat_value)
            os << "Array repeat " << to_string(e.repeat_count) << "\n";
        else
            os << "Array\n";
        ++depth;
        walk(e, *this);
        --depth;
    }

    // Struct
    void visit(StructExpression &e) override {
        indent();
        os << "Struct\n";
        ++depth;
        e.path->accept(*this);     // 路径一行
        for (auto &f : e.fields) { // 每个 Field 一行 + 值缩进
            indent();
            os << "Field " << f.name << "\n";
            ++depth;
            f.value->accept(*this);
            --depth;
        }
        --depth;
    }

    void visit(Function &e) override {
        // ret 非空约定：省略 ARROW 时构建器直接填 UnitType 实例，打印为 ()
        indent();
        os << "Function " << e.name << " -> " << e.ret->name() << "\n";
        ++depth;
        if (e.self_param) {
            indent();
            os << "Self " << name(e.self_param->kind) << "\n";
        }
        for (auto &p : e.params) {
            indent();
            os << "Param " << (p.is_mut ? "mut " : "") << p.name << ": "
               << p.type->name() << "\n";
        }
        walk(e, *this);
        --depth;
    }

    void visit(Struct &e) override {
        indent();
        os << "Struct " << e.name << "\n";
        ++depth;
        for (auto d : e.derives) {
            indent();
            os << "Derive " << name(d) << "\n";
        }
        for (auto &f : e.fields) {
            indent();
            os << "Field " << f.name << ": " << f.type->name() << "\n";
        }
        --depth;
    }

    void visit(Const &e) override {
        indent();
        os << "Const " << e.name << ": " << e.type->name() << " = "
           << to_string(e.value) << "\n";
    }

    void visit(InherentImpl &e) override {
        indent();
        os << "Impl " << e.self_type->name() << "\n";
        ++depth;
        walk(e, *this);
        --depth;
    }
};

} // namespace

void dump(Expression &expr, std::ostream &os) {
    Printer p{os};
    expr.accept(p);
}

void dump(Crate &crate, std::ostream &os) {
    Printer p{os};
    os << "Crate\n";
    ++p.depth;
    for (auto &item : crate.items)
        item->accept(p);
}

} // namespace rx
