//
// Urho3D Engine
// Copyright (c) 2008-2012 Lasse ��rni
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "Context.h"
#include "Deserializer.h"
#include "File.h"
#include "FileSystem.h"
#include "Graphics.h"
#include "GraphicsImpl.h"
#include "Log.h"
#include "Profiler.h"
#include "ResourceCache.h"
#include "Shader.h"
#include "ShaderVariation.h"
#include "XMLFile.h"

#include "DebugNew.h"

OBJECTTYPESTATIC(Shader);

Shader::Shader(Context* context) :
    Resource(context),
    sourceModifiedTime_(0)
{
}

Shader::~Shader()
{
}

void Shader::RegisterObject(Context* context)
{
    context->RegisterFactory<Shader>();
}

bool Shader::Load(Deserializer& source)
{
    PROFILE(LoadShader);
    
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;
    
    // Get absolute file name of the shader in case we need to invoke ShaderCompiler. This only works if the shader was not
    // loaded from a package file
    fullFileName_.Clear();
    sourceModifiedTime_ = 0;
    
    File* sourceFile = dynamic_cast<File*>(&source);
    if (sourceFile && !sourceFile->IsPackaged())
    {
        ResourceCache* cache = GetSubsystem<ResourceCache>();
        FileSystem* fileSystem = GetSubsystem<FileSystem>();
        if (cache && fileSystem && !fileSystem->HasRegisteredPaths())
        {
            fullFileName_ = cache->GetResourceFileName(GetName());
            if (!fullFileName_.Empty())
            {
                // Get last modified time of the main HLSL source file
                String path, fileName, extension;
                SplitPath(fullFileName_, path, fileName, extension);
                String hlslFileName = path + fileName + ".hlsl";
                sourceModifiedTime_ = fileSystem->GetLastModifiedTime(hlslFileName);
                
                // Check also timestamps of any included files
                if (sourceModifiedTime_)
                {
                    SharedPtr<File> file(new File(context_, hlslFileName));
                    while (file->IsOpen() && !file->IsEof())
                    {
                        String line = file->ReadLine();
                        if (line.StartsWith("#include"))
                        {
                            String includeFileName = path + line.Substring(9).Replaced("\"", "").Trimmed();
                            unsigned includeFileTime = fileSystem->GetLastModifiedTime(includeFileName);
                            if (includeFileTime > sourceModifiedTime_)
                                sourceModifiedTime_ = includeFileTime;
                        }
                    }
                }
                else
                {
                    // If the HLSL file was not found, do not attempt to compile shaders
                    fullFileName_.Clear();
                }
            }
        }
    }
    
    unsigned memoryUse = sizeof(Shader);
    
    SharedPtr<XMLFile> xml(new XMLFile(context_));
    if (!xml->Load(source))
        return false;
    
    ShaderParser vsParser;
    ShaderParser psParser;
    Vector<String> globalDefines;
    Vector<String> globalDefineValues;
    
    if (graphics->GetSM3Support())
    {
        globalDefines.Push("SM3");
        globalDefineValues.Push("1");
    }
    
    if (!vsParser.Parse(VS, xml->GetRoot("shaders"), globalDefines, globalDefineValues))
    {
        LOGERROR("VS: " + vsParser.GetErrorMessage());
        return false;
    }
    if (!psParser.Parse(PS, xml->GetRoot("shaders"), globalDefines, globalDefineValues))
    {
        LOGERROR("PS: " + psParser.GetErrorMessage());
        return false;
    }
    
    String path, fileName, extension;
    SplitPath(GetName(), path, fileName, extension);
    
    const Vector<ShaderCombination>& vsCombinations = vsParser.GetCombinations();
    for (Vector<ShaderCombination>::ConstIterator i = vsCombinations.Begin(); i != vsCombinations.End(); ++i)
    {
        StringHash nameHash(i->name_);
        String compiledShaderName = path + fileName;
        if (!i->name_.Empty())
            compiledShaderName += "_" + i->name_;
        compiledShaderName += graphics->GetSM3Support() ? ".vs3" : ".vs2";
        
        HashMap<StringHash, SharedPtr<ShaderVariation> >::Iterator j = vsVariations_.Find(nameHash);
        if (j == vsVariations_.End())
        {
            j = vsVariations_.Insert(MakePair(nameHash, SharedPtr<ShaderVariation>(new ShaderVariation(this, VS))));
            j->second_->SetName(compiledShaderName);
        }
        else
        {
            // If shader variation already exists, release and reset bytecode
            j->second_->Release();
            j->second_->SetByteCode(SharedArrayPtr<unsigned char>());
        }
    }
    
    const Vector<ShaderCombination>& psCombinations = psParser.GetCombinations();
    for (Vector<ShaderCombination>::ConstIterator i = psCombinations.Begin(); i != psCombinations.End(); ++i)
    {
        StringHash nameHash(i->name_);
        String compiledShaderName = path + fileName;
        if (!i->name_.Empty())
            compiledShaderName += "_" + i->name_;
        compiledShaderName += graphics->GetSM3Support() ? ".ps3" : ".ps2";
        
        HashMap<StringHash, SharedPtr<ShaderVariation> >::Iterator j = psVariations_.Find(nameHash);
        if (j == psVariations_.End())
        {
            j = psVariations_.Insert(MakePair(nameHash, SharedPtr<ShaderVariation>(new ShaderVariation(this, PS))));
            j->second_->SetName(compiledShaderName);
        }
        else
        {
            // If shader variation already exists, release and reset bytecode
            j->second_->Release();
            j->second_->SetByteCode(SharedArrayPtr<unsigned char>());
        }
    }
    
    memoryUse += (vsVariations_.Size() + psVariations_.Size()) * sizeof(ShaderVariation);
    SetMemoryUse(memoryUse);
    
    return true;
}

ShaderVariation* Shader::GetVariation(ShaderType type, const String& name)
{
    StringHash nameHash(name);
    
    if (type == VS)
    {
        HashMap<StringHash, SharedPtr<ShaderVariation> >::Iterator i = vsVariations_.Find(nameHash);
        if (i != vsVariations_.End())
            return i->second_;
        else
            return 0;
    }
    else
    {
        HashMap<StringHash, SharedPtr<ShaderVariation> >::Iterator i = psVariations_.Find(nameHash);
        if (i != psVariations_.End())
            return i->second_;
        else
            return 0;
    }
}

bool Shader::PrepareVariation(ShaderVariation* variation)
{
    if (!variation)
        return false;
    
    FileSystem* fileSystem = GetSubsystem<FileSystem>();
    Graphics* graphics = GetSubsystem<Graphics>();
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    if (!fileSystem || !graphics || !cache)
        return false;
    
    String shaderName = variation->GetName();
    String variationName = GetFileName(shaderName);
    unsigned pos = variationName.Find('_');
    if (pos != String::NPOS)
        variationName = variationName.Substring(pos + 1);
    else
        variationName.Clear();
    
    if (!fullFileName_.Empty())
    {
        if (fileSystem->GetLastModifiedTime(cache->GetResourceFileName(shaderName)) < sourceModifiedTime_)
        {
            PROFILE(CompileShader);
            
            LOGINFO("Compiling shader " + shaderName);
            
            Vector<String> arguments;
            arguments.Push("\"" + fullFileName_ + "\"");
            arguments.Push(variation->GetShaderType() == VS ? "-tVS" : "-tPS");
            arguments.Push("-v" + variationName);
            if (graphics->GetSM3Support())
                arguments.Push("-dSM3");
            
            if (fileSystem->SystemRun(fileSystem->GetProgramDir() + "ShaderCompiler", arguments) != 0)
            {
                LOGERROR("Failed to compile shader " + shaderName);
                return false;
            }
        }
    }
    
    SharedPtr<File> file(cache->GetFile(shaderName));
    if (!file)
        return false;
    
    if (file->ReadFileID() != "USHD")
    {
        LOGERROR(shaderName + " is not a valid shader bytecode file");
        return false;
    }
    /// \todo Check that shader type and model match
    unsigned short shaderType = file->ReadUShort();
    unsigned short shaderModel = file->ReadUShort();
    
    unsigned numParameters = file->ReadUInt();
    for (unsigned i = 0; i < numParameters; ++i)
    {
        String paramName = file->ReadString();
        unsigned reg = file->ReadUByte();
        unsigned regCount = file->ReadUByte();
        
        StringHash nameHash(paramName);
        ShaderParameter parameter(variation->GetShaderType(), reg, regCount);
        variation->AddParameter(nameHash, parameter);
        
        // Register the parameter globally
        graphics->RegisterShaderParameter(nameHash, parameter);
    }
    
    unsigned numTextureUnits = file->ReadUInt();
    for (unsigned i = 0; i < numTextureUnits; ++i)
    {
        String unitName = file->ReadString();
        unsigned sampler = file->ReadUByte();
        
        TextureUnit tuIndex = graphics->GetTextureUnit(unitName);
        if (tuIndex != MAX_TEXTURE_UNITS)
            variation->AddTextureUnit(tuIndex);
        else if (sampler < MAX_TEXTURE_UNITS)
            variation->AddTextureUnit((TextureUnit)sampler);
    }
    
    unsigned byteCodeSize = file->ReadUInt();
    if (byteCodeSize)
    {
        SharedArrayPtr<unsigned char> byteCode(new unsigned char[byteCodeSize]);
        file->Read(byteCode.Get(), byteCodeSize);
        variation->SetByteCode(byteCode);
        SetMemoryUse(GetMemoryUse() + byteCodeSize);
        return true;
    }
    else
    {
        LOGERROR("Shader " + shaderName + " has zero length bytecode");
        return false;
    }
}
