
#pragma once

class Script;

typedef void (*_CaptureLambdaVars)(Script* scriptLambda);
extern _CaptureLambdaVars CaptureLambdaVars;
typedef void (*_UncaptureLambdaVars)(Script* scriptLambda);
extern _UncaptureLambdaVars UncaptureLambdaVars;

class LambdaVariableContext
{
	Script* scriptLambda;
public:
	LambdaVariableContext(const LambdaVariableContext& other) = delete;
	LambdaVariableContext& operator=(const LambdaVariableContext& other) = delete;
	LambdaVariableContext(Script* scriptLambda);
	LambdaVariableContext(LambdaVariableContext&& other) noexcept;
	LambdaVariableContext& operator=(LambdaVariableContext&& other) noexcept;
	~LambdaVariableContext();

	friend bool operator==(const LambdaVariableContext& lhs, const LambdaVariableContext& rhs);

	friend bool operator!=(const LambdaVariableContext& lhs, const LambdaVariableContext& rhs);

	Script*& operator*();

	Script* operator*() const;

	explicit operator bool() const
	{
		return scriptLambda != nullptr;
	}
};