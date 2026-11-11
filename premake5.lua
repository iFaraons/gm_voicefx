newoption({
	trigger = "gmcommon",
	description = "Sets the path to the garrysmod_common (https://github.com/danielga/garrysmod_common) directory",
	value = "path to garrysmod_common directory"
})

local gmcommon = assert(_OPTIONS.gmcommon or os.getenv("GARRYSMOD_COMMON"),
	"you didn't provide a path to your garrysmod_common (https://github.com/danielga/garrysmod_common) directory")
include(gmcommon .. "/generator.v3.lua")

CreateWorkspace({name = "voicefx"})
	CreateProject({serverside = true})
		cppdialect("C++17")

		IncludeSDKCommon()
		IncludeSDKTier0()
		IncludeSDKTier1()
		IncludeDetouring()
		IncludeScanning()
		IncludeLuaShared()
		IncludeHelpersExtended()

		links("opus")
		includedirs("opus/include")

		filter({"platforms:x86_64"})
			libdirs({"opus/lib64"})

		filter({"platforms:x86"})
			libdirs({"opus/lib32"})

		-- не зависеть от версии libstdc++ на сервере
		filter({"system:linux"})
			linkoptions({"-static-libstdc++", "-static-libgcc"})
