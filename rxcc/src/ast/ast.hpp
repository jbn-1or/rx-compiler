#pragma once
#include <cstdint>
#include <string>
#include <iosfwd>
#include <memory>
#include <vector>

namespace rx {

// 源码位置：由 ANTLR token 的 getLine() / getCharPositionInLine() 转换而来。
// 行从 1 开始，列从 0 开始，与 ANTLR 一致
struct Position {
    int line;    // 行号，从 1 起
    int column;  // 列号，从 0 起
};

// 源码区间 [start, end)：一个节点在源文件中覆盖的范围，用于报错定位。
struct Span {
    Position start;  // 区间起点（含）
    Position end;    // 区间终点（不含）
};

struct AstVisitor;

// 所有 AST 节点的共同基类
struct Node {
    Span span;  // 该节点对应的源码区间

    virtual ~Node() = default;

    virtual void accept(AstVisitor&) = 0;
};

struct Type;

// 表达式分类层
struct Expression : Node {
    Type* type = nullptr;
};

// 语句分类层
struct Statement : Node {};

// 整数后缀，对齐 Lexer.g4 的 INTEGER_SUFFIX；None = 无后缀。
enum class IntSuffix { None, I32, U32, Isize, Usize };

// 二元运算符
enum class BinOp {
    Add, Sub, Mul, Div, Rem,
    BitAnd, BitXor, BitOr, Shl, Shr,
    Eq, Ne, Lt, Le, Gt, Ge,
    LogAnd, LogOr,
};

enum class UnOp { Neg, Not, Deref, Borrow, BorrowMut };

// 名称解析槽位取值
enum class Res { Unresolved, Local, TopFn, ConstItem, AssocItem, SelfValue };

enum class AssignOp {
    Assign, AddAssign, SubAssign, MulAssign, DivAssign, RemAssign,
    BitAndAssign, BitXorAssign, BitOrAssign, ShlAssign, ShrAssign,
};

// 路径段（纯值）：名字 + 可选类型泛型实参（Box::<i32>::new）。
struct PathSegment {
    std::string name;                             // identifier / self / Self
    std::vector<std::shared_ptr<Type>> generics{};
};

// 常量值（纯值，非节点）：[x; N] 的 N 与 const 项初始化器共用
struct ConstValue {
    enum class Kind { Int, Bool, Path, Neg };
    Kind kind = Kind::Int;
    int64_t int_value = 0;                // Int
    bool bool_value = false;              // Bool
    std::vector<PathSegment> path;        // Path：常量项路径
    std::shared_ptr<ConstValue> operand;  // Neg 的操作数
};

std::string to_string(const ConstValue& v);   // 打印与报错

// —— Type 层次 ——（非 visitor 节点）
struct Type {
    Span span;   // 该类型在源码中写出的区间

    virtual ~Type() = default;
    virtual std::string name() const = 0;   // 打印与报错
    virtual bool equals(const Type& other) const = 0;
};

// () ；函数省略返回类型时
struct UnitType : Type {
    std::string name() const override { return "()"; }
    bool equals(const Type& other) const override;
};

// 路径类型：基本类型/结构体/Box/Vec/Self。段与 PathExpression 共用 PathSegment
struct PathType : Type {
    std::vector<PathSegment> segments;
    std::string name() const override;
    bool equals(const Type& other) const override;
};

// 引用类型：&T / &mut T。&&T 是两层嵌套
struct ReferenceType : Type {
    bool is_mut = false;
    std::shared_ptr<Type> inner;
    std::string name() const override;
    bool equals(const Type& other) const override;
};

// 数组类型：[T; N]。N 复用 ConstValue。
struct ArrayType : Type {
    std::shared_ptr<Type> element; 
    ConstValue length;
    std::string name() const override;
    bool equals(const Type& other) const override;
};

const char* name(BinOp op);
const char* name(UnOp op);
const char* name(AssignOp op);

// derive 名单的枚举（纯值）：outerAttribute 的 #[derive(..)]。
enum class Derive { Copy, Clone, PartialEq, Eq };
const char* name(Derive d);

// self 参数的四种形态（纯值）：self / mut self / &self / &mut self
struct SelfParam {
    enum class Kind { Value, MutValue, Ref, RefMut };
    Kind kind = Kind::Value;
};
const char* name(SelfParam::Kind k);   // "self"/"mut self"/"&self"/"&mut self"

// 函数普通参数 ：identifierBinding COLON typeRef。
struct Param {
    std::string name;
    bool is_mut = false;
    std::shared_ptr<Type> type = nullptr;
};

// 结构体字段声明 ：identifier COLON typeRef。
struct StructField {
    std::string name;
    std::shared_ptr<Type> type = nullptr;
};

struct IntLit;
struct BinaryExpression;
struct BoolLit;
struct UnitExpression;
struct PathExpression;
struct UnaryExpression;
struct AssignmentExpression;
struct CastExpression;
struct CallExpression;
struct MethodCallExpression;
struct FieldExpression;
struct IndexExpression;
struct BlockExpression;
struct LetStatement;
struct ExpressionStatement;
struct IfExpression;
struct LoopExpression;
struct WhileExpression;
struct BreakExpression;
struct ContinueExpression;
struct ReturnExpression;
struct ArrayExpression;
struct StructExpression;
struct Function;
struct Struct;
struct Const;
struct InherentImpl;

struct AstVisitor {
    virtual ~AstVisitor() = default;
    virtual void visit(IntLit&) = 0;
    virtual void visit(BinaryExpression&) = 0;
    virtual void visit(BoolLit&) = 0;
    virtual void visit(UnitExpression&) = 0;
    virtual void visit(PathExpression&) = 0;
    virtual void visit(UnaryExpression& e) = 0;
    virtual void visit(AssignmentExpression& e) = 0;
    virtual void visit(CastExpression& e) = 0;
    virtual void visit(CallExpression&) = 0;
    virtual void visit(MethodCallExpression&) = 0;
    virtual void visit(FieldExpression&) = 0;
    virtual void visit(IndexExpression&) = 0;
    virtual void visit(BlockExpression&) = 0;
    virtual void visit(LetStatement&) = 0;
    virtual void visit(ExpressionStatement&) = 0;
    virtual void visit(IfExpression&) = 0;
    virtual void visit(LoopExpression&) = 0;
    virtual void visit(WhileExpression&) = 0;
    virtual void visit(BreakExpression&) = 0;
    virtual void visit(ContinueExpression&) = 0;
    virtual void visit(ReturnExpression&) = 0;
    virtual void visit(ArrayExpression&) = 0;
    virtual void visit(StructExpression&) = 0;
    virtual void visit(Function&) = 0;
    virtual void visit(Struct&) = 0;
    virtual void visit(Const&) = 0;
    virtual void visit(InherentImpl&) = 0;
};

// 整数字面量
// value = 数值；raw = 原始拼写
struct IntLit : Expression {
    int64_t value;
    std::string raw;
    IntSuffix suffix;

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 二元表达式：op + 左右操作数。
struct BinaryExpression : Expression {
    BinOp op;
    std::shared_ptr<Expression> lhs;  // 左操作数（先求值）
    std::shared_ptr<Expression> rhs;  // 右操作数

    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct BoolLit : Expression {
    bool value;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 单元值 ()：空括号
struct UnitExpression : Expression {
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 一元表达式：前缀运算符递归（- -x → Neg(Neg(x))）。
struct UnaryExpression : Expression {
    UnOp op;
    std::shared_ptr<Expression> operand;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 赋值：lhs 是 place右结合—— a = b = c 的树是 Assign(a, Assign(b, c))。
struct AssignmentExpression : Expression {
    AssignOp op;
    std::shared_ptr<Expression> lhs;  // 目标 place
    std::shared_ptr<Expression> rhs;  // 右侧值
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// as 转换：左折叠，x as T as U → Cast(Cast(x,T),U)。
struct CastExpression : Expression {
    std::shared_ptr<Expression> operand;
    std::shared_ptr<Type> target;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 调用：callee 是任意表达式（f、f(1)、Type::method 多段路径……），不退化为字符串。
struct CallExpression : Expression {
    std::shared_ptr<Expression> callee;
    std::vector<std::shared_ptr<Expression>> args;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 方法调用：x.m(...)。method 是路径段：名字 + 可选 turbofish（DOT pathExprSegment）。
struct MethodCallExpression : Expression {
    std::shared_ptr<Expression> receiver;
    PathSegment method;
    std::vector<std::shared_ptr<Expression>> args;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 字段访问：x.f
struct FieldExpression : Expression {
    std::shared_ptr<Expression> base;
    std::string field;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 索引：a[i]
struct IndexExpression : Expression {
    std::shared_ptr<Expression> base;
    std::shared_ptr<Expression> index;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 块表达式：语句序列 + 可选尾表达式。
// 块的值 = tail 的值；无 tail 类型为 unit
// tail 是独立字段，statement* statementExpression?
struct BlockExpression : Expression {
    std::vector<std::shared_ptr<Statement>> statements;
    std::shared_ptr<Expression> tail;   // 可空：无尾表达式的块
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// let 语句：init 语法强制必有（equalsSign expression 无 ?）。
struct LetStatement : Statement {
    std::string name;
    bool is_mut = false;               // identifierBinding 的 MUT?
    std::shared_ptr<Type> type_annotation;   // 可空
    std::shared_ptr<Expression> init;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 表达式语句：分号不进 AST
// 包住任意表达式，含省略分号的块类表达式
struct ExpressionStatement : Statement {
    std::shared_ptr<Expression> expr;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// if 表达式：条件 + then 块 + 可空 else（块或 if——else if 链天然嵌套）。
// then 用具体类型 BlockExpression：语法强制是块，字段能具体就具体；
// else_ 必须用 Expression：ELSE 后面是块或 if 两种可能。
struct IfExpression : Expression {
    std::shared_ptr<Expression> condition;
    std::shared_ptr<BlockExpression> then;
    std::shared_ptr<Expression> else_;   // 可空
    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct LoopExpression : Expression {
    std::shared_ptr<BlockExpression> body;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// while：条件每次迭代前重新求值（规范 1964）——walk 顺序 condition 在前。
struct WhileExpression : Expression {
    std::shared_ptr<Expression> condition;
    std::shared_ptr<BlockExpression> body;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// break：value 可空（break; → unit；带值 break 决定 loop 结果类型，规范 1968）。
struct BreakExpression : Expression {
    std::shared_ptr<Expression> value;   // 可空
    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct ContinueExpression : Expression {
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// return：value 可空；类型是 never（规范 1337），语义阶段回填。
struct ReturnExpression : Expression {
    std::shared_ptr<Expression> value;   // 可空
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 数组表达式：两形——列表 [a, b]（含空 []）与重复 [x; N]。
// 靠 repeat_value 是否为空区分；重复形 elem 只求值一次（规范 1825）。
struct ArrayExpression : Expression {
    std::vector<std::shared_ptr<Expression>> elements;   // 列表形
    std::shared_ptr<Expression> repeat_value;            // 重复形：唯一的元素表达式
    ConstValue repeat_count;                             // 重复形：常量次数
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 结构体字段的"名字+值"对（纯值，对齐产生式 structExprField）。
struct StructExprField {
    std::string name;
    std::shared_ptr<Expression> value;
};

// 结构体表达式：path 复用 PathExpression——语义阶段解析结构体名
// 就靠它的 res 槽位
struct StructExpression : Expression {
    std::shared_ptr<PathExpression> path;
    std::vector<StructExprField> fields;   // 可空（S {}）
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 路径表达式：多段合法（a::b::c）。res 是名称解析槽位，构建时 Unresolved。
struct PathExpression : Expression {
    std::vector<PathSegment> segments;   // 至少一段
    Res res = Res::Unresolved;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// —— Item 层：顶层与 impl 内的声明 ——
struct Item : Node {};

// 函数：body 语法强制必有（无 `;` 声明形）。ret 可空 = 省略，语义上是 unit
struct Function : Item {
    std::string name;
    std::shared_ptr<SelfParam> self_param;      // 可空：普通函数无 self
    std::vector<Param> params;
    std::shared_ptr<Type> ret;              // 省略 ARROW 时构建器直接填 UnitType 实例
    std::shared_ptr<BlockExpression> body;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 结构体定义：derives + 字段表（名字+类型都是纯值）。
struct Struct : Item {
    std::string name;
    std::vector<StructField> fields;
    std::vector<Derive> derives;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 常量项：value 复用 ConstValue
struct Const : Item {
    std::string name;
    std::shared_ptr<Type> type;      // const identifier COLON typeRef
    ConstValue value;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// inherent impl：items 只出现 Function/Const；self_type 对齐 IMPL 后的 typeRef。
struct InherentImpl : Item {
    std::shared_ptr<Type> self_type;
    std::vector<std::shared_ptr<Item>> items;
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// crate：AST 的根。装节点
struct Crate {
    std::vector<std::shared_ptr<Item>> items;
};

// 按规范求值顺序遍历 node 的子节点
void walk(Node& node, AstVisitor& visitor);

// 调试打印：一行一节点、子节点缩进一级
void dump(Expression& expr, std::ostream& os);
void dump(Crate& crate, std::ostream& os);   // 根容器：手动迭代 items

} // namespace rx
