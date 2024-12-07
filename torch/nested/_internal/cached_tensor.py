from contextlib import contextmanager
from typing import *  # noqa: F403

import torch
from torch.utils import _pytree as pytree


class CachedTensor(torch.Tensor):
    @staticmethod
    def __new__(
        cls,
        metadata: Dict[str, torch.Tensor],
        source_field: str,
    ) -> "CachedTensor":
        source = metadata[source_field]
        shape = source.shape
        kwargs = {}
        kwargs["strides"] = source.stride()
        kwargs["storage_offset"] = source.storage_offset()  # type: ignore[assignment]
        kwargs["device"] = source.device  # type: ignore[assignment]
        kwargs["layout"] = source.layout  # type: ignore[assignment]
        kwargs["requires_grad"] = source.requires_grad  # type: ignore[assignment]
        kwargs["dtype"] = source.dtype  # type: ignore[assignment]
        out = torch.Tensor._make_wrapper_subclass(cls, shape, **kwargs)  # type: ignore[attr-defined]
        return out

    def __init__(
        self,
        metadata: Dict[str, torch.Tensor],
        source_field: str,
    ):
        self.source_field = source_field
        self.metadata = metadata

        # Compile only
        self.nested_int_ref: Any = None

    def __repr__(self) -> str:  # type: ignore[override]
        return f"CachedTensor({repr(self.metadata[self.source_field])})"

    def __getattr__(self, name: str) -> torch.Tensor:
        if name in self.metadata:
            return self.metadata[name]
        else:
            raise AttributeError(
                f"{type(self).__name__} object has no attribute '{name}'"
            )

    def __tensor_flatten__(self) -> Tuple[List[str], Dict[str, Any]]:
        ctx = {
            "source_field": self.source_field,
        }
        return list(self.metadata.keys()), ctx

    @staticmethod
    def __tensor_unflatten__(
        inner_tensors: Dict, meta: Dict, outer_size: Any, outer_stride: Any
    ) -> "CachedTensor":
        return CachedTensor(inner_tensors, source_field=meta["source_field"])

    @classmethod
    def __torch_dispatch__(
        cls, op: Any, types: Any, args: Any, kwargs: Any
    ) -> torch.Tensor:
        # Ensure that any registered ops are loaded
        import torch.nested._internal.ops  # noqa: F401

        if kwargs is None:
            kwargs = {}

        if op in _func_registry:
            return _func_registry[op](op, *args, **kwargs)

        # By default, doing any operation on a CachedTensor automatically unwraps and
        # returns a non-CachedTensor
        unwrapped_args = pytree.tree_map_only(
            CachedTensor,
            lambda x: x.metadata[x.source_field],
            args,
        )
        unwrapped_kwargs = pytree.tree_map_only(
            CachedTensor,
            lambda x: x.metadata[x.source_field],
            kwargs,
        )
        return op(*unwrapped_args, **unwrapped_kwargs)


torch.serialization.add_safe_globals([CachedTensor])


_func_registry: Dict[Any, Callable] = {}


@contextmanager
def set_func_registry(registry: Dict[Any, Callable]) -> Generator:
    global _func_registry
    # Save the current registry to restore it later
    old_registry = _func_registry
    # Set the new registry
    _func_registry = registry
    try:
        yield
    finally:
        # Restore the old registry
        _func_registry = old_registry


# Note: [ CacheTensor open registry ]
#
# Registering this decorator for an op allows CachedTensor's torch dispatch to
# redirect calls to that op to your own implementations.
# For NestedTensor we rely on this behavior for factory functions.
def register_cached_tensor_func(aten_op: Any) -> Callable:
    def wrapper(func: Callable) -> Callable:
        _func_registry[aten_op] = func
        return func

    return wrapper