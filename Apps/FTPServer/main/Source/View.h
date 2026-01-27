#pragma once

class View {
public:

    virtual ~View() = default;
    virtual void onStop() = 0;
};
