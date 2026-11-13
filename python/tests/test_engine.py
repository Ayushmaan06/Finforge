import finforge


def test_engine_version_comes_from_cpp() -> None:
    v = finforge.engine_version()
    semver, _, rest = v.partition("+")
    assert semver == finforge.__version__
    assert rest.endswith(".Release"), v


def test_extension_is_compiled() -> None:
    from finforge import _engine

    assert _engine.__file__.endswith(".so")
