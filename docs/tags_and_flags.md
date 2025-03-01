## Tags and Flags

The game files identify countries by a case-sensitive, three-letter code, generally known as their tag. Tags aren't very useful to us, as internally we convert those references to arbitrary consecutive ids. However, in some rare places we may need to be able to recover that three-letter string to display to the user. Bytes storing the letters of the tag are stored in the `identifying_int` property of a national identity. You can use the `int_to_tag` function to turn that number back into a string as follows:
```c++
nations::int_to_tag(state.world.national_identity_get_identifying_int(id_for_a_national_identity));
```
Remember that a nation in the game is not, for us, the same thing as a tag. To get the tag associated with a particular nation you need to first go through the `identity_holder` relationship.

### Flags

All the flag types will be added to the vector `state::flag_type_names` which associates a `flag_type_id` and a `text_key` for the flag type in question. This is created at scenario time and will stay static through the entire game.

The flag to display for a nation is determined by two factors: the national identity associated with the nation and its current government type. The current type of government for a nation is found in a nation's `government_type` property. Once you have the national identity and the type of government you can then look up the type of flag we are supposed to display using the following function:
```c++
dcon::flag_type_id culture::get_current_flag_type(sys::state const& state, dcon::nation_id target_nation);
```
With that information you can then call `GLuint ogl::get_flag_handle(sys::state& state, dcon::national_identity_id nat_id, dcon::flag_type_id type);` which will return a handle to the flag texture (loading it from disk if necessary).

Because the lookups required to determine which flag to display are a bit convoluted, it is probably best to cache either the combination of national identity and flag type or the handle to the texture itself and then recalculate those values upon receiving the `update` message. 
