import methods


def make_scripts(target, source, env):
    with methods.generated_wrapper(str(target[0])) as output:
        for name, path in zip(["youtube_ejs_lib", "youtube_ejs_core"], source):
            with open(str(path), encoding="utf-8") as script:
                output.write("static const char " + name + "[] =\n" + methods.to_raw_cstring(script.read()) + ";\n")
