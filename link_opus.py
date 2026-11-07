Import("env")
env.Append(LIBPATH=[env.subst("$PROJECT_DIR/vendor")], LIBS=["opus"])
