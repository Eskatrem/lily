enum class Option[A] {
	Some(A),
	None
}

# This tests that type inference is able to look at the type that is wanted
# to fill in generics that are unknown by any of the variants in the list.
list[Option[integer]] k = [None, None, None]
