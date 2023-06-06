import Flow

@_expose(Cxx)
public struct ExposeVoidConf<T> {
    let x: CInt
}

// FIXME: return Void? for conformance instead once header supports stdlib types.
@_expose(Cxx)
// This function ensures that the value witness table for `Void` to C++ is
// exposed in the generated C++ header.
public func _exposeVoidValueTypeConformanceToCpp(_ val: ExposeVoidConf<Void>)  {
}

@_expose(Cxx)
public struct FlowCheckedContinuation<T> {
    public typealias CC = CheckedContinuation<T, Swift.Error>
    var cc: CC?

    public init() {}

    public init(_ cc: CC) {
        self.cc = cc
    }

    public mutating func set(_ other: FlowCheckedContinuation<T>) {
        // precondition: other continuation must be set.
        assert(other.cc != nil)
        cc = other.cc
    }

    public func resume(returning value: T) {
        // precondition: continuation must be set.
        assert(cc != nil)
        cc!.resume(returning: value)
    }

    public func resumeThrowing(_ value: Flow.Error) {
        // precondition: continuation must be set.
        assert(cc != nil)
        // TODO: map errors.
        cc!.resume(throwing: GeneralFlowError(value))
    }
}

public struct GeneralFlowError: Swift.Error {
    let underlying: Flow.Error?

    public init() {
        self.underlying = nil
    }

    public init(_ underlying: Flow.Error) {
        self.underlying = underlying
    }
}
