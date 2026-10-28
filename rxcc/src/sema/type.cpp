#include "sema/type.hpp"
#include "sema/symbol.hpp"

namespace rx {

std::string SemanticType::toString() const {
    const auto inner_name = [&] { return inner ? inner->toString() : "<error>"; };
    switch (kind) {
        case Kind::Unit:
            return "()";
        case Kind::Never:
            return "!";
        case Kind::Error:
            return "<error>";
        case Kind::Int:
            switch (int_kind) {
                case IntKind::I32: return "i32";
                case IntKind::U32: return "u32";
                case IntKind::Isize: return "isize";
                case IntKind::Usize: return "usize";
            }
            break;
        case Kind::Bool:
            return "bool";
        case Kind::Struct:
            return struct_symbol ? struct_symbol->name : "<error>";
        case Kind::Reference:
            return (is_mut ? "&mut " : "&") + inner_name();
        case Kind::Array:
            return "[" + inner_name() + "; " + std::to_string(length) + "]";
        case Kind::Box:
            return "Box<" + inner_name() + ">";
        case Kind::Vec:
            return "Vec<" + inner_name() + ">";
    }
    return "<error>";
}

} // namespace rx
