import Foundation
import TinyA

// A wrapper (DESIGN 6.6): the library's module with the product's define, an option as a compilation
// condition, a resource.
public func tinyAReport() -> String {
#if OTOK_OPTION_FAST
    let mode = "fast"
#else
    let mode = "slow"
#endif
    let url = Bundle.module.url(forResource: "greeting", withExtension: "txt")
    let greeting = url.flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? "?"
    return "\(tiny_a()) \(tiny_a_extra()) \(tiny_a_zlib()) \(mode) \(greeting.trimmingCharacters(in: .whitespacesAndNewlines))"
}
